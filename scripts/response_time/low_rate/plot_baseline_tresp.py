"""Plot the RK3588/RK3568 2x3 T_resp^max baseline comparison."""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import plot_3588_all_conditions as base


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
DATA_ROOT = MATERIAL_ROOT / "data" / "response_time" / "low_rate"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "appendix" / "response_time"
SCENARIOS = ("stress", "idle")
SCENARIO_LABELS = {
    "idle": "Inter-core interference",
    "stress": "Inter-core + intra-core interference",
}
FREQUENCIES = (1000, 100, 10)
DISPLAY_LABELS = {
    "Native": "Baseline",
    "WP": "WP",
    "SP": "SP",
    "HCP_4-16": r"$C^{3}$",
}

# Common paper labels -> result-directory configuration names.
PLATFORMS = {
    "RK3588": {
        "results": DATA_ROOT / "rk3588" / "results",
        "conditions": {
            "Native": "Native",
            "SP": "SP",
            "WP": "WP",
            "HCP_4-16": "HCP_4-16",
        },
    },
    "RK3568": {
        "results": DATA_ROOT / "rk3568" / "results",
        "conditions": {
            "Native": "Native",
            "SP": "SP02",
            "WP": "WP",
            "HCP_4-16": "HCP",
        },
    },
}

# Exact colors and line/marker choices used by plot_repeated_experiment.py.
STYLES = {
    "Native": {
        "color": "#222222", "marker": "o", "linestyle": "-",
        "linewidth": 1.6, "markersize": 3.2, "zorder": 8,
    },
    "WP": {
        "color": "#B36B2C", "marker": "s", "linestyle": "-",
        "linewidth": 1.6, "markersize": 3.2, "zorder": 7,
    },
    "SP": {
        "color": "#4A8C5A", "marker": "^", "linestyle": "-",
        "linewidth": 1.6, "markersize": 3.2, "zorder": 7,
    },
    "HCP_4-16": {
        "color": "#1F5A99", "marker": "D", "linestyle": "-",
        "linewidth": 2.0, "markersize": 3.4, "zorder": 10,
    },
}
METHOD_ORDER = ("Native", "WP", "SP", "HCP_4-16")


def condition_name(config: str, scenario: str, frequency: int) -> str:
    return f"{config}_rtos_{scenario}_{frequency}Hz"


def collect_stats() -> tuple[dict[str, list[dict]], list[str]]:
    by_platform = {}
    all_vmbytes = set()
    for platform, spec in PLATFORMS.items():
        records, issues = base.scan_records(spec["results"])
        stats = base.aggregate(records)
        by_platform[platform] = stats
        all_vmbytes.update(item["vmbytes"] for item in stats if item["metric"] == "t2_max")
        print(
            f"[Loaded] {platform}: complete_runs={len(records)}, "
            f"excluded_issues={len(issues)}"
        )
    ordered = [label for label in base.VM_BYTES_ORDER if label in all_vmbytes]
    extras = sorted(all_vmbytes - set(ordered), key=base.vm_to_bytes)
    return by_platform, ordered + extras


def plot_panel(
    ax,
    stats: list[dict],
    platform: str,
    scenario: str,
    frequency: int,
    vm_labels: list[str],
    metric: str = "t2_max",
) -> None:
    condition_map = PLATFORMS[platform]["conditions"]
    by_key = {
        (item["condition"], item["vmbytes"]): item
        for item in stats
        if item["metric"] == metric
    }
    plotted = 0
    for method in METHOD_ORDER:
        style = STYLES[method]
        condition = condition_name(condition_map[method], scenario, frequency)
        labels = [label for label in vm_labels if (condition, label) in by_key]
        if not labels:
            print(f"[Missing] {platform} {frequency}Hz {method}: {condition}")
            continue

        points = [by_key[(condition, label)] for label in labels]
        x_values = np.asarray([base.vm_x(label) for label in labels])
        medians = np.asarray([point["median"] for point in points])
        minima = np.asarray([point["min"] for point in points])
        maxima = np.asarray([point["max"] for point in points])

        if any(point["n"] > 1 for point in points):
            ax.fill_between(
                x_values, minima, maxima,
                color=style["color"], alpha=0.12, linewidth=0,
                zorder=style["zorder"] - 2,
            )
        ax.plot(
            x_values,
            medians,
            label=DISPLAY_LABELS[method],
            color=style["color"],
            marker=style["marker"],
            linestyle="-",
            linewidth=style["linewidth"],
            markersize=style["markersize"],
            markerfacecolor=style["color"] if method == "HCP_4-16" else "white",
            markeredgewidth=0.8,
            zorder=style["zorder"],
        )
        plotted += 1

    if not plotted:
        ax.text(0.5, 0.5, "No completed data", ha="center", va="center", transform=ax.transAxes)
    ax.set_xticks([base.vm_x(label) for label in vm_labels])
    ax.set_xticklabels(
        [base.display_tick(label) for label in vm_labels], rotation=45, ha="right"
    )
    ax.grid(axis="y", linestyle="--", linewidth=0.45, alpha=0.35)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)
    ax.ticklabel_format(style="plain", axis="y")

def collect_legend(axes) -> tuple[list, list[str]]:
    by_label = {}
    for ax in np.asarray(axes).ravel():
        handles, labels = ax.get_legend_handles_labels()
        for handle, label in zip(handles, labels):
            by_label.setdefault(label, handle)
    labels = [
        DISPLAY_LABELS[method]
        for method in METHOD_ORDER
        if DISPLAY_LABELS[method] in by_label
    ]
    return [by_label[label] for label in labels], labels


def share_row_y_limits(axes) -> None:
    """Use one y range across all frequencies of each platform row."""
    for row in range(axes.shape[0]):
        populated = [ax for ax in axes[row, :] if ax.lines]
        if not populated:
            continue
        lower = min(ax.get_ylim()[0] for ax in populated)
        upper = max(ax.get_ylim()[1] for ax in populated)
        for ax in axes[row, :]:
            ax.set_ylim(lower, upper)


def plot_scenario(
    stats_by_platform: dict[str, list[dict]],
    vm_labels: list[str],
    scenario: str,
) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(13.2, 5.5), sharex=True)
    for column, frequency in enumerate(FREQUENCIES):
        axes[0, column].set_title(f"{frequency} Hz")
    for row, platform in enumerate(PLATFORMS):
        for column, frequency in enumerate(FREQUENCIES):
            plot_panel(
                axes[row, column],
                stats_by_platform[platform],
                platform,
                scenario,
                frequency,
                vm_labels,
            )
        axes[row, 0].set_ylabel(
            r"Maximum response time, $T_{\mathrm{resp}}^{\max}$ ($\mu$s)"
        )
        axes[row, 0].annotate(
            platform,
            xy=(-0.30, 0.5),
            xycoords="axes fraction",
            rotation=90,
            ha="center",
            va="center",
            fontsize=10,
            fontweight="bold",
        )
    for ax in axes[0]:
        ax.tick_params(labelbottom=False)
    for ax in axes[1]:
        ax.set_xlabel("Aggregate GPOS working-set size")
    share_row_y_limits(axes)

    handles, labels = collect_legend(axes)
    if handles:
        fig.legend(
            handles, labels, loc="lower center", ncol=len(labels),
            frameon=False, bbox_to_anchor=(0.5, 0.005),
        )
    fig.tight_layout(rect=(0.025, 0.075, 1, 0.995), pad=0.35)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / f"rk3588_rk3568_tresp_max_{scenario}_2x3"
    fig.savefig(
        basename.with_suffix(".png"), dpi=300, bbox_inches="tight", pad_inches=0.02
    )
    fig.savefig(basename.with_suffix(".pdf"), bbox_inches="tight", pad_inches=0.02)
    plt.close(fig)
    print(f"[Saved] {basename.with_suffix('.png')}")
    print(f"[Saved] {basename.with_suffix('.pdf')}")


def plot_1000hz_scenarios(
    stats_by_platform: dict[str, list[dict]],
    vm_labels: list[str],
) -> None:
    """Plot both interference scenarios at 1000 Hz in a paper-ready 2x2 layout."""
    scenario_order = ("idle", "stress")
    fig, axes = plt.subplots(2, 2, figsize=(9.2, 5.8), sharex=True)

    for column, scenario in enumerate(scenario_order):
        axes[0, column].set_title(SCENARIO_LABELS[scenario])
    for row, platform in enumerate(PLATFORMS):
        for column, scenario in enumerate(scenario_order):
            plot_panel(
                axes[row, column],
                stats_by_platform[platform],
                platform,
                scenario,
                1000,
                vm_labels,
                metric="t2_jitter",
            )
        axes[row, 0].set_ylabel(
            r"$J_{\mathrm{resp}}$ ($\mu\mathrm{s}$)"
        )
        axes[row, 0].annotate(
            platform,
            xy=(-0.29, 0.5),
            xycoords="axes fraction",
            rotation=90,
            ha="center",
            va="center",
            fontsize=10,
            fontweight="bold",
        )
    for ax in axes[0]:
        ax.tick_params(labelbottom=False)
    for ax in axes[1]:
        ax.set_xlabel("Aggregate GPOS working-set size")

    handles, labels = collect_legend(axes)
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
    fig.tight_layout(rect=(0.035, 0.095, 1, 0.995), pad=0.35)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / "rk3588_rk3568_jresp_1000Hz_2x2"
    fig.savefig(
        basename.with_suffix(".png"), dpi=300, bbox_inches="tight", pad_inches=0.02
    )
    fig.savefig(basename.with_suffix(".pdf"), bbox_inches="tight", pad_inches=0.02)
    plt.close(fig)
    print(f"[Saved] {basename.with_suffix('.png')}")
    print(f"[Saved] {basename.with_suffix('.pdf')}")


def maximum_tresp_rows(
    stats_by_platform: dict[str, list[dict]],
) -> list[dict[str, str | int | float]]:
    """Return maxima across working-set sizes and repetitions for every condition."""
    rows = []
    for platform in PLATFORMS:
        stats = stats_by_platform[platform]
        condition_map = PLATFORMS[platform]["conditions"]
        for scenario in ("idle", "stress"):
            for frequency in FREQUENCIES:
                row: dict[str, str | int | float] = {
                    "Platform": platform,
                    "Interference": SCENARIO_LABELS[scenario],
                    "Frequency (Hz)": frequency,
                }
                for method in METHOD_ORDER:
                    condition = condition_name(
                        condition_map[method], scenario, frequency
                    )
                    values = [
                        item["max"]
                        for item in stats
                        if item["metric"] == "t2_max"
                        and item["condition"] == condition
                    ]
                    row[DISPLAY_LABELS[method].replace("$", "").replace("^{3}", "3")] = (
                        max(values) if values else float("nan")
                    )
                rows.append(row)
    return rows


def print_and_save_maximum_tresp_table(
    stats_by_platform: dict[str, list[dict]],
) -> None:
    rows = maximum_tresp_rows(stats_by_platform)
    fields = [
        "Platform",
        "Interference",
        "Frequency (Hz)",
        "Baseline",
        "WP",
        "SP",
        "C3",
    ]
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output_path = OUTPUT_DIR / "maximum_tresp_by_condition.csv"
    with output_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    formatted_rows = []
    for row in rows:
        formatted_rows.append(
            [
                str(row["Platform"]),
                str(row["Interference"]),
                str(row["Frequency (Hz)"]),
                *[
                    "N/A" if np.isnan(float(row[method])) else f"{float(row[method]):.2f}"
                    for method in ("Baseline", "WP", "SP", "C3")
                ],
            ]
        )
    widths = [
        max(len(field), *(len(row[index]) for row in formatted_rows))
        for index, field in enumerate(fields)
    ]
    print("\nMaximum T_resp (us) across all working-set sizes and repetitions")
    print(" | ".join(field.ljust(widths[index]) for index, field in enumerate(fields)))
    print("-+-".join("-" * width for width in widths))
    for row in formatted_rows:
        print(" | ".join(value.ljust(widths[index]) for index, value in enumerate(row)))
    print(f"[Saved] {output_path}")


def print_1000hz_jresp_data(
    stats_by_platform: dict[str, list[dict]],
    vm_labels: list[str],
) -> None:
    """Print the aggregated 1000 Hz t2_jitter values in analysis-ready CSV form."""
    print("\n1000 Hz J_resp data (us)")
    print("Platform,Interference,WorkingSet,Method,Median,Min,Max,N")
    for platform in PLATFORMS:
        condition_map = PLATFORMS[platform]["conditions"]
        by_key = {
            (item["condition"], item["vmbytes"]): item
            for item in stats_by_platform[platform]
            if item["metric"] == "t2_jitter"
        }
        for scenario in ("idle", "stress"):
            for vm_label in vm_labels:
                for method in METHOD_ORDER:
                    condition = condition_name(
                        condition_map[method], scenario, 1000
                    )
                    point = by_key.get((condition, vm_label))
                    if point is None:
                        continue
                    print(
                        f"{platform},{SCENARIO_LABELS[scenario]},{vm_label},"
                        f"{DISPLAY_LABELS[method].replace('$', '').replace('^{3}', '3')},"
                        f"{point['median']:.3f},{point['min']:.3f},"
                        f"{point['max']:.3f},{point['n']}"
                    )


def main() -> int:
    base.setup_matplotlib()
    stats_by_platform, vm_labels = collect_stats()
    if not vm_labels:
        raise RuntimeError("No complete t2_max records were found")
    for scenario in SCENARIOS:
        plot_scenario(stats_by_platform, vm_labels, scenario)
    plot_1000hz_scenarios(stats_by_platform, vm_labels)
    print_and_save_maximum_tresp_table(stats_by_platform)
    print_1000hz_jresp_data(stats_by_platform, vm_labels)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
