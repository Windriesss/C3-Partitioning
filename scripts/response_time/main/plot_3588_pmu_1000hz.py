"""Plot RK3588 PMU trends at 1000 Hz for both interference scenarios."""

from __future__ import annotations

from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import plot_3588_all_conditions as base
import plot_baseline_tresp as baseline


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
RESULTS_DIR = MATERIAL_ROOT / "data" / "response_time" / "main" / "rk3588" / "results"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "main"
FREQUENCY = 1000

SCENARIOS = (
    ("idle", "Inter-core interference"),
    ("stress", "Inter-core + intra-core interference"),
)
PMU_METRICS = (
    ("avg_l2_inval", "Avg. L2D cache invalidations"),
    ("avg_l2_refill", "Avg. L2D cache refills"),
    ("avg_ll_miss_rd", "Avg. L3 read misses"),
)
CONDITIONS = baseline.PLATFORMS["RK3588"]["conditions"]


def available_vm_labels(stats: list[dict]) -> list[str]:
    labels = {
        item["vmbytes"]
        for item in stats
        if item["metric"] in {metric for metric, _ in PMU_METRICS}
    }
    ordered = [label for label in base.VM_BYTES_ORDER if label in labels]
    extras = sorted(labels - set(ordered), key=base.vm_to_bytes)
    return ordered + extras


def plot_panel(
    ax,
    stats: list[dict],
    metric: str,
    scenario: str,
    vm_labels: list[str],
) -> None:
    by_key = {
        (item["condition"], item["vmbytes"]): item
        for item in stats
        if item["metric"] == metric
    }
    for method in baseline.METHOD_ORDER:
        condition = baseline.condition_name(CONDITIONS[method], scenario, FREQUENCY)
        labels = [label for label in vm_labels if (condition, label) in by_key]
        if not labels:
            print(f"[Missing] RK3588 {FREQUENCY}Hz {scenario} {method}: {condition}")
            continue

        points = [by_key[(condition, label)] for label in labels]
        x_values = np.asarray([base.vm_x(label) for label in labels])
        medians = np.asarray([point["median"] for point in points])
        minima = np.asarray([point["min"] for point in points])
        maxima = np.asarray([point["max"] for point in points])
        style = baseline.STYLES[method]

        if any(point["n"] > 1 for point in points):
            ax.fill_between(
                x_values,
                minima,
                maxima,
                color=style["color"],
                alpha=0.12,
                linewidth=0,
                zorder=style["zorder"] - 2,
            )
        ax.plot(
            x_values,
            medians,
            label=baseline.DISPLAY_LABELS[method],
            color=style["color"],
            marker=style["marker"],
            linestyle="-",
            linewidth=style["linewidth"],
            markersize=style["markersize"],
            markerfacecolor=style["color"] if method == "HCP_4-16" else "white",
            markeredgewidth=0.8,
            zorder=style["zorder"],
        )

    ax.set_xticks([base.vm_x(label) for label in vm_labels])
    ax.set_xticklabels(
        [base.display_tick(label) for label in vm_labels],
        rotation=45,
        ha="right",
    )
    ax.ticklabel_format(style="plain", axis="y")
    ax.grid(axis="y", linestyle="--", linewidth=0.45, alpha=0.35)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)
    ax.margins(x=0.02)


def plot_figure(stats: list[dict], vm_labels: list[str]) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(10.2, 5.7), sharex=True)

    for row, (scenario, scenario_label) in enumerate(SCENARIOS):
        for column, (metric, metric_label) in enumerate(PMU_METRICS):
            ax = axes[row, column]
            plot_panel(ax, stats, metric, scenario, vm_labels)
            if row == 0:
                ax.set_title(metric_label, fontsize=9)
        axes[row, 0].annotate(
            scenario_label,
            xy=(-0.34, 0.5),
            xycoords="axes fraction",
            rotation=90,
            ha="center",
            va="center",
            fontsize=9,
            fontweight="bold",
        )

    for ax in axes[0]:
        ax.tick_params(labelbottom=False)
    for ax in axes[1]:
        ax.set_xlabel("Aggregate GPOS working-set size")

    handles, labels = baseline.collect_legend(axes)
    if handles:
        fig.legend(
            handles,
            labels,
            loc="lower center",
            ncol=len(labels),
            frameon=False,
            bbox_to_anchor=(0.5, 0.004),
            fontsize=10,
            handlelength=1.5,
            columnspacing=1.2,
            handletextpad=0.45,
            borderaxespad=0.0,
        )

    fig.tight_layout(rect=(0.025, 0.095, 1, 0.995), pad=0.35)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / "rk3588_pmu_1000Hz_2x3"
    fig.savefig(
        basename.with_suffix(".png"),
        dpi=300,
        bbox_inches="tight",
        pad_inches=0.02,
    )
    fig.savefig(
        basename.with_suffix(".pdf"),
        bbox_inches="tight",
        pad_inches=0.02,
    )
    plt.close(fig)
    print(f"[Saved] {basename.with_suffix('.png')}")
    print(f"[Saved] {basename.with_suffix('.pdf')}")


def main() -> int:
    base.setup_matplotlib()
    records, issues = base.scan_records(RESULTS_DIR)
    print(
        f"[Loaded] RK3588: complete_runs={len(records)}, "
        f"excluded_issues={len(issues)}"
    )
    if not records:
        raise RuntimeError("No complete RK3588 rerun records were found")

    stats = base.aggregate(records)
    vm_labels = available_vm_labels(stats)
    if not vm_labels:
        raise RuntimeError("No RK3588 PMU data were found")
    plot_figure(stats, vm_labels)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
