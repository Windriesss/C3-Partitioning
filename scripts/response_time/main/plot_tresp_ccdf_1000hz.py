"""Plot 1000 Hz T_resp CCDFs for RK3588 and RK3568.

Each curve pools the final cumulative t2 histogram from every complete run and
working-set size belonging to the same platform, method, and interference
scenario.  RK3568's ``HCP`` result directory is the platform-specific mapping
of the paper's HCP4-16/C^3 method.
"""

from __future__ import annotations

import csv
import json
import re
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import plot_3588_all_conditions as base


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
DATA_ROOT = MATERIAL_ROOT / "data" / "response_time" / "main"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "main"
FREQUENCY = 1000

PLATFORMS = {
    "RK3588": {
        "results": DATA_ROOT / "rk3588" / "results",
        "configs": {
            "Native": "Native",
            "WP": "WP",
            "SP": "SP",
            "HCP4-16": "HCP_4-16",
        },
    },
    "RK3568": {
        "results": DATA_ROOT / "rk3568" / "results",
        "configs": {
            "Native": "Native",
            "WP": "WP",
            "SP": "SP02",
            "HCP4-16": "HCP",
        },
    },
}

METHOD_ORDER = ("Native", "WP", "SP", "HCP4-16")
SCENARIOS = ("idle", "stress")
SCENARIO_LABELS = {
    "idle": "Inter-core interference",
    "stress": "Inter-core + intra-core interference",
}
DISPLAY_LABELS = {
    "Native": "Baseline",
    "WP": "WP",
    "SP": "SP",
    "HCP4-16": r"$C^3$",
}
STYLES = {
    "Native": {"color": "#222222", "linestyle": "-", "linewidth": 1.7},
    "WP": {"color": "#B36B2C", "linestyle": "--", "linewidth": 1.7},
    "SP": {"color": "#4A8C5A", "linestyle": "-.", "linewidth": 1.7},
    "HCP4-16": {"color": "#1F5A99", "linestyle": "-", "linewidth": 2.1},
}

T2_REGION_RE = re.compile(r"(?ms)^t2_region>\s*(.*?)(?=^\s*t0_min:)")
T2_BIN_RE = re.compile(
    r"^\s*\[([0-9]+(?:\.[0-9]+)?)\]\s*:\s*(\d+)\s*$", re.M
)


def condition_name(config: str, scenario: str) -> str:
    return f"{config}_rtos_{scenario}_{FREQUENCY}Hz"


def parse_complete_histogram(
    summary_path: Path, condition: str
) -> tuple[dict[float, int] | None, str | None]:
    """Read the last complete cumulative t2 histogram for one run."""
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, f"cannot read summary: {exc}"

    if summary.get("status") != "complete":
        return None, f"summary status={summary.get('status')}"
    if summary.get("experiment") != condition:
        return None, "summary experiment does not match its directory"

    expected_samples = int(summary.get("minimum_pmu_samples") or 0)
    if expected_samples <= 0:
        return None, "summary has no positive minimum_pmu_samples"

    prefix = summary_path.name.removesuffix("_summary.json")
    rtos_path = summary_path.with_name(f"{prefix}_rtos.log")
    if not rtos_path.is_file():
        return None, f"missing {rtos_path.name}"

    text = base.current_rtos_run(
        rtos_path.read_text(encoding="utf-8", errors="ignore")
    )
    block = base.last_complete_block(text, expected_samples)
    if block is None:
        return None, f"no complete block with >= {expected_samples} samples"

    match = T2_REGION_RE.search(block)
    if match is None:
        return None, "complete block has no t2_region"

    histogram: dict[float, int] = {}
    for bin_text, count_text in T2_BIN_RE.findall(match.group(1)):
        # RTOS assigns integer-us buckets by truncation.  Plot bucket centres.
        value = float(bin_text) + 0.5
        histogram[value] = histogram.get(value, 0) + int(count_text)
    if not histogram or sum(histogram.values()) <= 0:
        return None, "t2_region has no samples"
    return histogram, None


def collect_histograms() -> tuple[dict[tuple[str, str, str], dict], list[dict]]:
    """Pool complete runs over the working sets shared by all four methods."""
    pooled: dict[tuple[str, str, str], dict] = {}
    issues: list[dict] = []

    for platform, platform_spec in PLATFORMS.items():
        results_root = platform_spec["results"]
        for scenario in SCENARIOS:
            condition_dirs = {
                method: results_root
                / condition_name(platform_spec["configs"][method], scenario)
                for method in METHOD_ORDER
            }
            vm_sets = [
                {
                    child.name
                    for child in condition_dir.iterdir()
                    if child.is_dir() and child.name.startswith("vm_")
                }
                for condition_dir in condition_dirs.values()
                if condition_dir.is_dir()
            ]
            common_vm_dirs = set.intersection(*vm_sets) if len(vm_sets) == 4 else set()
            print(
                f"[Scope] {platform} {scenario}: "
                f"{len(common_vm_dirs)} common working-set sizes"
            )

            for method in METHOD_ORDER:
                config = platform_spec["configs"][method]
                condition = condition_name(config, scenario)
                condition_dir = condition_dirs[method]
                combined: defaultdict[float, int] = defaultdict(int)
                sources: list[str] = []

                if not condition_dir.is_dir():
                    issues.append(
                        {
                            "platform": platform,
                            "scenario": scenario,
                            "method": method,
                            "source": condition_dir.relative_to(MATERIAL_ROOT).as_posix(),
                            "reason": "condition directory is missing",
                        }
                    )
                else:
                    summary_paths = (
                        summary_path
                        for vm_dir_name in sorted(common_vm_dirs)
                        for summary_path in sorted(
                            (condition_dir / vm_dir_name).glob("*_summary.json")
                        )
                    )
                    for summary_path in summary_paths:
                        histogram, error = parse_complete_histogram(
                            summary_path, condition
                        )
                        if error is not None:
                            issues.append(
                                {
                                    "platform": platform,
                                    "scenario": scenario,
                                    "method": method,
                                    "source": summary_path.relative_to(MATERIAL_ROOT).as_posix(),
                                    "reason": error,
                                }
                            )
                            continue
                        for value, count in histogram.items():
                            combined[value] += count
                        sources.append(summary_path.relative_to(MATERIAL_ROOT).as_posix())

                pooled[(platform, scenario, method)] = {
                    "histogram": dict(combined),
                    "runs": len(sources),
                    "samples": sum(combined.values()),
                    "sources": sources,
                }
                print(
                    f"[Loaded] {platform} {scenario} {method}: "
                    f"runs={len(sources)}, samples={sum(combined.values()):,}"
                )
    return pooled, issues


def ccdf(histogram: dict[float, int]) -> tuple[np.ndarray, np.ndarray]:
    """Return bucket centres and P(T_resp >= x) for a weighted histogram."""
    items = sorted(histogram.items())
    x = np.asarray([value for value, _ in items], dtype=float)
    counts = np.asarray([count for _, count in items], dtype=np.int64)
    tail_counts = np.cumsum(counts[::-1], dtype=np.int64)[::-1]
    return x, tail_counts / tail_counts[0]


def save_curve_data(pooled: dict[tuple[str, str, str], dict]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output_path = OUTPUT_DIR / "rk3588_rk3568_tresp_ccdf_1000Hz_data.csv"
    fields = [
        "Platform",
        "Interference",
        "Method",
        "T_resp bin center (us)",
        "Bin count",
        "CCDF P(T_resp >= x)",
        "Runs pooled",
        "Samples pooled",
    ]
    with output_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for platform in PLATFORMS:
            for scenario in SCENARIOS:
                for method in METHOD_ORDER:
                    item = pooled[(platform, scenario, method)]
                    histogram = item["histogram"]
                    if not histogram:
                        continue
                    x, y = ccdf(histogram)
                    for value, probability in zip(x, y):
                        writer.writerow(
                            {
                                "Platform": platform,
                                "Interference": SCENARIO_LABELS[scenario],
                                "Method": method,
                                "T_resp bin center (us)": f"{value:.3f}",
                                "Bin count": histogram[float(value)],
                                "CCDF P(T_resp >= x)": f"{probability:.12g}",
                                "Runs pooled": item["runs"],
                                "Samples pooled": item["samples"],
                            }
                        )
    print(f"[Saved] {output_path}")


def save_issues(issues: list[dict]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output_path = OUTPUT_DIR / "rk3588_rk3568_tresp_ccdf_1000Hz_issues.csv"
    fields = ["platform", "scenario", "method", "source", "reason"]
    with output_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(issues)
    print(f"[Saved] {output_path} ({len(issues)} issues)")


def plot(pooled: dict[tuple[str, str, str], dict]) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(9.4, 6.2), sharey=True)

    for column, scenario in enumerate(SCENARIOS):
        axes[0, column].set_title(SCENARIO_LABELS[scenario])

    for row, platform in enumerate(PLATFORMS):
        row_x_min: list[float] = []
        row_x_max: list[float] = []
        for column, scenario in enumerate(SCENARIOS):
            ax = axes[row, column]
            for method in METHOD_ORDER:
                item = pooled[(platform, scenario, method)]
                if not item["histogram"]:
                    continue
                x, y = ccdf(item["histogram"])
                style = STYLES[method]
                ax.step(
                    x,
                    y,
                    where="post",
                    label=DISPLAY_LABELS[method],
                    color=style["color"],
                    linestyle=style["linestyle"],
                    linewidth=style["linewidth"],
                    zorder=5 if method == "HCP4-16" else 3,
                )
                row_x_min.append(float(x.min()))
                row_x_max.append(float(x.max()))

            ax.set_yscale("log")
            ax.set_ylim(bottom=1e-7, top=1.05)
            ax.grid(True, which="major", linestyle="--", linewidth=0.5, alpha=0.40)
            ax.grid(True, which="minor", axis="y", linestyle=":", linewidth=0.35, alpha=0.25)
            ax.spines["top"].set_visible(False)
            ax.spines["right"].set_visible(False)
            ax.tick_params(direction="out", length=3, width=0.7)
            if row == len(PLATFORMS) - 1:
                ax.set_xlabel(r"$T_{\mathrm{resp}}$ ($\mu$s)")
            if column == 0:
                ax.set_ylabel(r"CCDF, $\Pr(T_{\mathrm{resp}}\geq x)$")
            ax.annotate(
                platform,
                xy=(-0.23, 0.5),
                xycoords="axes fraction",
                rotation=90,
                ha="center",
                va="center",
                fontsize=10,
                fontweight="bold",
            )

        if row_x_min and row_x_max:
            lower = min(row_x_min)
            upper = max(row_x_max)
            padding = max((upper - lower) * 0.035, 0.5)
            for ax in axes[row]:
                ax.set_xlim(lower - padding, upper + padding)

    handles_by_label = {}
    for ax in axes.ravel():
        handles, labels = ax.get_legend_handles_labels()
        for handle, label in zip(handles, labels):
            handles_by_label.setdefault(label, handle)
    labels = [
        DISPLAY_LABELS[method]
        for method in METHOD_ORDER
        if DISPLAY_LABELS[method] in handles_by_label
    ]
    fig.legend(
        [handles_by_label[label] for label in labels],
        labels,
        loc="lower center",
        ncol=len(labels),
        frameon=False,
        bbox_to_anchor=(0.5, 0.005),
    )
    fig.tight_layout(rect=(0.035, 0.085, 1, 0.995), pad=0.55)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / "rk3588_rk3568_tresp_ccdf_1000Hz_2x2"
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
    pooled, issues = collect_histograms()
    if not any(item["histogram"] for item in pooled.values()):
        raise RuntimeError("No complete t2 histograms were found")
    plot(pooled)
    save_curve_data(pooled)
    save_issues(issues)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
