"""Plot 1000-Hz J_resp with run-level bootstrap confidence bands.

The four methods are retained even when fewer than five completed runs are
available.  For a cell with N >= 5, the point is the run-level median and the
shaded region is a 95% percentile-bootstrap interval for that median.
Cells below the target repetition count are plotted without a band; no
interval is fabricated.
"""

from __future__ import annotations

import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import plot_3588_all_conditions as base
import plot_baseline_tresp as baseline


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "main"
OUTPUT_STEM = "rk3588_rk3568_jresp_1000Hz_2x2"
BOOTSTRAP_RESAMPLES = 20_000
RANDOM_SEED = 20260729
TARGET_RUNS = 5
METRIC = "t2_jitter"


def bootstrap_median_ci(
    values: np.ndarray,
    rng: np.random.Generator,
) -> tuple[float, float]:
    """Return the 2.5th and 97.5th percentiles of bootstrapped medians."""
    if values.size < TARGET_RUNS:
        return math.nan, math.nan
    samples = rng.choice(
        values,
        size=(BOOTSTRAP_RESAMPLES, values.size),
        replace=True,
    )
    medians = np.median(samples, axis=1)
    low, high = np.percentile(medians, [2.5, 97.5])
    return float(low), float(high)


def collect_cells() -> tuple[dict[str, list[dict]], list[str]]:
    """Parse complete runs and summarize each platform/condition/working set."""
    rng = np.random.default_rng(RANDOM_SEED)
    cells_by_platform: dict[str, list[dict]] = {}
    all_vmbytes: set[str] = set()

    for platform, spec in baseline.PLATFORMS.items():
        records, issues = base.scan_records(spec["results"])
        grouped: dict[tuple[str, str], list[float]] = defaultdict(list)
        for record in records:
            value = float(record.get(METRIC, math.nan))
            if math.isfinite(value):
                grouped[(record["condition"], record["vmbytes"])].append(value)

        cells = []
        for (condition, vmbytes), raw_values in sorted(grouped.items()):
            values = np.asarray(raw_values, dtype=float)
            ci_low, ci_high = bootstrap_median_ci(values, rng)
            cells.append(
                {
                    "condition": condition,
                    "vmbytes": vmbytes,
                    "n": int(values.size),
                    "median": float(np.median(values)),
                    "ci_low": ci_low,
                    "ci_high": ci_high,
                    "minimum": float(np.min(values)),
                    "maximum": float(np.max(values)),
                }
            )
            all_vmbytes.add(vmbytes)

        cells_by_platform[platform] = cells
        print(
            f"[Loaded] {platform}: complete_runs={len(records)}, "
            f"excluded_issues={len(issues)}"
        )

    ordered = [label for label in base.VM_BYTES_ORDER if label in all_vmbytes]
    extras = sorted(all_vmbytes - set(ordered), key=base.vm_to_bytes)
    return cells_by_platform, ordered + extras


def plot_panel(
    ax,
    cells: list[dict],
    platform: str,
    scenario: str,
    vm_labels: list[str],
) -> None:
    condition_map = baseline.PLATFORMS[platform]["conditions"]
    by_key = {(cell["condition"], cell["vmbytes"]): cell for cell in cells}

    for method in baseline.METHOD_ORDER:
        style = baseline.STYLES[method]
        condition = baseline.condition_name(condition_map[method], scenario, 1000)
        labels = [label for label in vm_labels if (condition, label) in by_key]
        if not labels:
            continue

        points = [by_key[(condition, label)] for label in labels]
        x = np.asarray([base.vm_x(label) for label in labels])
        y = np.asarray([point["median"] for point in points])

        # Draw a confidence band only at cells meeting the target repetition
        # count.  The mask prevents the band from bridging unsupported cells.
        eligible = np.asarray(
            [
                point["n"] >= TARGET_RUNS
                and math.isfinite(point["ci_low"])
                and math.isfinite(point["ci_high"])
                for point in points
            ],
            dtype=bool,
        )
        if np.any(eligible):
            ci_low = np.asarray([
                point["ci_low"] if ok else np.nan
                for point, ok in zip(points, eligible)
            ])
            ci_high = np.asarray([
                point["ci_high"] if ok else np.nan
                for point, ok in zip(points, eligible)
            ])
            ax.fill_between(
                x,
                ci_low,
                ci_high,
                where=eligible,
                interpolate=False,
                color=style["color"],
                alpha=0.11 if method != "HCP_4-16" else 0.14,
                linewidth=0,
                zorder=style["zorder"] - 2,
            )

        # Keep the point estimates on the exact working-set positions and
        # render them above the uncertainty band.
        ax.plot(
            x,
            y,
            label=baseline.DISPLAY_LABELS[method],
            color=style["color"],
            marker=style["marker"],
            linestyle=style["linestyle"],
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
    ax.grid(axis="y", linestyle="--", linewidth=0.45, alpha=0.35)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)
    ax.ticklabel_format(style="plain", axis="y")


def save_csv(cells_by_platform: dict[str, list[dict]], vm_labels: list[str]) -> Path:
    output = OUTPUT_DIR / f"{OUTPUT_STEM}_bootstrap95ci_data.csv"
    fields = [
        "platform",
        "scenario",
        "working_set",
        "method",
        "n_runs",
        "median_jresp_us",
        "ci95_low_us",
        "ci95_high_us",
        "minimum_us",
        "maximum_us",
        "interval_status",
    ]
    rows = []
    for platform, cells in cells_by_platform.items():
        condition_map = baseline.PLATFORMS[platform]["conditions"]
        by_key = {(cell["condition"], cell["vmbytes"]): cell for cell in cells}
        for scenario in ("idle", "stress"):
            for method in baseline.METHOD_ORDER:
                condition = baseline.condition_name(
                    condition_map[method], scenario, 1000
                )
                for vmbytes in vm_labels:
                    cell = by_key.get((condition, vmbytes))
                    if cell is None:
                        continue
                    has_ci = cell["n"] >= TARGET_RUNS
                    rows.append(
                        {
                            "platform": platform,
                            "scenario": scenario,
                            "working_set": vmbytes,
                            "method": baseline.DISPLAY_LABELS[method]
                            .replace("$", "")
                            .replace("^{3}", "3"),
                            "n_runs": cell["n"],
                            "median_jresp_us": f"{cell['median']:.9g}",
                            "ci95_low_us": (
                                f"{cell['ci_low']:.9g}" if has_ci else ""
                            ),
                            "ci95_high_us": (
                                f"{cell['ci_high']:.9g}" if has_ci else ""
                            ),
                            "minimum_us": f"{cell['minimum']:.9g}",
                            "maximum_us": f"{cell['maximum']:.9g}",
                            "interval_status": (
                                "run-level percentile bootstrap"
                                if has_ci
                                else f"not shown (n<{TARGET_RUNS})"
                            ),
                        }
                    )

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return output


def main() -> int:
    base.setup_matplotlib()
    # This 2x2 figure is used at nearly full paper-column width, so keep the
    # typography readable after scaling while avoiding oversized outer margins.
    plt.rcParams.update(
        {
            "font.size": 11,
            "axes.labelsize": 12,
            "axes.titlesize": 12,
            "xtick.labelsize": 10,
            "ytick.labelsize": 10,
            "legend.fontsize": 12,
        }
    )
    cells_by_platform, vm_labels = collect_cells()
    if not vm_labels:
        raise RuntimeError("No complete 1000-Hz J_resp records were found")

    scenario_order = ("idle", "stress")
    fig, axes = plt.subplots(2, 2, figsize=(9.8, 5.8), sharex=True)
    for column, scenario in enumerate(scenario_order):
        axes[0, column].set_title(baseline.SCENARIO_LABELS[scenario])

    for row, platform in enumerate(baseline.PLATFORMS):
        for column, scenario in enumerate(scenario_order):
            plot_panel(
                axes[row, column],
                cells_by_platform[platform],
                platform,
                scenario,
                vm_labels,
            )
        axes[row, 0].set_ylabel(r"$J_{\mathrm{resp}}$ ($\mu\mathrm{s}$)")
        axes[row, 0].annotate(
            platform,
            xy=(-0.205, 0.5),
            xycoords="axes fraction",
            rotation=90,
            ha="center",
            va="center",
            fontsize=12,
            fontweight="bold",
        )

    for ax in axes[0]:
        ax.tick_params(labelbottom=False)
    for ax in axes[1]:
        ax.set_xlabel("Aggregate GPOS working-set size")

    handles, labels = baseline.collect_legend(axes)
    fig.legend(
        handles,
        labels,
        loc="lower center",
        ncol=len(labels),
        frameon=False,
        bbox_to_anchor=(0.5, 0.004),
        fontsize=12,
        handlelength=1.7,
        columnspacing=1.35,
        handletextpad=0.5,
        borderaxespad=0.0,
    )
    fig.subplots_adjust(
        left=0.105,
        right=0.995,
        top=0.955,
        bottom=0.205,
        wspace=0.08,
        hspace=0.04,
    )

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / OUTPUT_STEM
    png = basename.with_suffix(".png")
    pdf = basename.with_suffix(".pdf")
    fig.savefig(png, dpi=300, bbox_inches="tight", pad_inches=0.01)
    fig.savefig(pdf, bbox_inches="tight", pad_inches=0.01)
    plt.close(fig)

    csv_path = save_csv(cells_by_platform, vm_labels)
    print(f"[Saved] {png}")
    print(f"[Saved] {pdf}")
    print(f"[Saved] {csv_path}")
    print(
        f"[Note] 95% bootstrap confidence bands require at least {TARGET_RUNS} runs."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
