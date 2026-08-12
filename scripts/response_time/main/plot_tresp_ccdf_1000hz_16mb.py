"""Plot 1000 Hz T_resp CCDFs at the fixed 16 MiB pressure size."""

from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import plot_3588_all_conditions as base
import plot_tresp_ccdf_1000hz as common


VM_DIR = "vm_16M"
VM_LABEL = "16 MiB"
OUTPUT_DIR = common.MATERIAL_ROOT / "figures" / "main"
OUTPUT_STEM = "rk3588_rk3568_tresp_ccdf_1000Hz_16MiB_2x2"


def collect() -> tuple[dict[tuple[str, str, str], dict], list[dict]]:
    pooled: dict[tuple[str, str, str], dict] = {}
    issues: list[dict] = []

    for platform, platform_spec in common.PLATFORMS.items():
        for scenario in common.SCENARIOS:
            for method in common.METHOD_ORDER:
                config = platform_spec["configs"][method]
                condition = common.condition_name(config, scenario)
                vm_dir = platform_spec["results"] / condition / VM_DIR
                combined: defaultdict[float, int] = defaultdict(int)
                sources: list[str] = []

                if not vm_dir.is_dir():
                    issues.append(
                        {
                            "platform": platform,
                            "scenario": scenario,
                            "method": method,
                            "source": vm_dir.relative_to(common.MATERIAL_ROOT).as_posix(),
                            "reason": "16 MiB result directory is missing",
                        }
                    )
                else:
                    for summary_path in sorted(vm_dir.glob("*_summary.json")):
                        histogram, error = common.parse_complete_histogram(
                            summary_path, condition
                        )
                        if error is not None:
                            issues.append(
                                {
                                    "platform": platform,
                                    "scenario": scenario,
                                    "method": method,
                                    "source": summary_path.relative_to(common.MATERIAL_ROOT).as_posix(),
                                    "reason": error,
                                }
                            )
                            continue
                        for value, count in histogram.items():
                            combined[value] += count
                        sources.append(summary_path.relative_to(common.MATERIAL_ROOT).as_posix())

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


def save_data(pooled: dict[tuple[str, str, str], dict]) -> None:
    output_path = OUTPUT_DIR / f"{OUTPUT_STEM}_data.csv"
    fields = [
        "Platform",
        "Interference",
        "Pressure size",
        "Method",
        "T_resp bin center (us)",
        "Bin count",
        "CCDF P(T_resp >= x)",
        "Runs",
        "Samples",
    ]
    with output_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for platform in common.PLATFORMS:
            for scenario in common.SCENARIOS:
                for method in common.METHOD_ORDER:
                    item = pooled[(platform, scenario, method)]
                    histogram = item["histogram"]
                    if not histogram:
                        continue
                    x, y = common.ccdf(histogram)
                    for value, probability in zip(x, y):
                        writer.writerow(
                            {
                                "Platform": platform,
                                "Interference": common.SCENARIO_LABELS[scenario],
                                "Pressure size": VM_LABEL,
                                "Method": method,
                                "T_resp bin center (us)": f"{value:.3f}",
                                "Bin count": histogram[float(value)],
                                "CCDF P(T_resp >= x)": f"{probability:.12g}",
                                "Runs": item["runs"],
                                "Samples": item["samples"],
                            }
                        )
    print(f"[Saved] {output_path}")


def save_issues(issues: list[dict]) -> None:
    output_path = OUTPUT_DIR / f"{OUTPUT_STEM}_issues.csv"
    fields = ["platform", "scenario", "method", "source", "reason"]
    with output_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(issues)
    print(f"[Saved] {output_path} ({len(issues)} issues)")


def plot(pooled: dict[tuple[str, str, str], dict]) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(8.2, 5.25), sharey=True)
    for column, scenario in enumerate(common.SCENARIOS):
        axes[0, column].set_title(common.SCENARIO_LABELS[scenario])

    for row, platform in enumerate(common.PLATFORMS):
        row_min: list[float] = []
        row_max: list[float] = []
        for column, scenario in enumerate(common.SCENARIOS):
            ax = axes[row, column]
            for method in common.METHOD_ORDER:
                histogram = pooled[(platform, scenario, method)]["histogram"]
                if not histogram:
                    continue
                x, y = common.ccdf(histogram)
                style = common.STYLES[method]
                ax.step(
                    x,
                    y,
                    where="post",
                    label=common.DISPLAY_LABELS[method],
                    color=style["color"],
                    linestyle=style["linestyle"],
                    linewidth=style["linewidth"],
                    zorder=5 if method == "HCP4-16" else 3,
                )
                row_min.append(float(x.min()))
                row_max.append(float(x.max()))

            ax.set_yscale("log")
            ax.set_ylim(1e-6, 1.05)
            ax.grid(True, which="major", linestyle="--", linewidth=0.5, alpha=0.4)
            ax.grid(
                True, which="minor", axis="y",
                linestyle=":", linewidth=0.35, alpha=0.25,
            )
            ax.spines["top"].set_visible(False)
            ax.spines["right"].set_visible(False)
            ax.tick_params(direction="out", length=3, width=0.7)
            if row == 1:
                ax.set_xlabel(r"$T_{\mathrm{resp}}$ ($\mu$s)")
            if column == 0:
                ax.set_ylabel(r"CCDF, $\Pr(T_{\mathrm{resp}}\geq x)$")
            if column == 0:
                ax.annotate(
                    platform,
                    xy=(-0.205, 0.5),
                    xycoords="axes fraction",
                    rotation=90,
                    ha="center",
                    va="center",
                    fontsize=10,
                    fontweight="bold",
                    annotation_clip=False,
                )

        if row_min:
            lower, upper = min(row_min), max(row_max)
            padding = max((upper - lower) * 0.035, 0.5)
            for ax in axes[row]:
                ax.set_xlim(lower - padding, upper + padding)

    handles_by_label = {}
    for ax in axes.ravel():
        handles, labels = ax.get_legend_handles_labels()
        for handle, label in zip(handles, labels):
            handles_by_label.setdefault(label, handle)
    labels = [
        common.DISPLAY_LABELS[method]
        for method in common.METHOD_ORDER
        if common.DISPLAY_LABELS[method] in handles_by_label
    ]
    fig.legend(
        [handles_by_label[label] for label in labels],
        labels,
        loc="lower center",
        ncol=len(labels),
        frameon=False,
        bbox_to_anchor=(0.5, 0.012),
        fontsize=9.5,
        handlelength=1.45,
        columnspacing=1.05,
        handletextpad=0.4,
        borderaxespad=0.0,
    )
    fig.subplots_adjust(
        left=0.13,
        right=0.992,
        top=0.925,
        bottom=0.155,
        wspace=0.12,
        hspace=0.16,
    )

    basename = OUTPUT_DIR / OUTPUT_STEM
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
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    pooled, issues = collect()
    if not any(item["histogram"] for item in pooled.values()):
        raise RuntimeError("No complete 16 MiB t2 histograms were found")
    plot(pooled)
    save_data(pooled)
    save_issues(issues)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
