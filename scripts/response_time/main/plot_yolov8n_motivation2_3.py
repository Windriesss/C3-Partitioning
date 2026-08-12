"""Plot RK3588 YOLOv8n Motivation 2/3 response-time jitter results."""

from __future__ import annotations

import csv
import json
import math
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import plot_3588_all_conditions as base


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
DATA_DIR = MATERIAL_ROOT / "data" / "response_time" / "main" / "rk3588" / "result_yolov8n"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "main"

# Motivation 2/3 uses the high-rate PMU experiment.
FREQUENCY_HZ = 1000
METHODS = ("Native", "WP", "SP")
METHOD_LABELS = {
    "Native": "Baseline",
    "WP": "WP",
    "SP": "SP",
}
CONDITIONS = {
    "oee_idle_rtos_idle": "Idle",
    "oee_yolov8n_rtos_idle": "Inter-core interference",
    "oee_yolov8n_rtos_stress": "Inter-core + intra-core interference",
}
PRINTED_METRICS = (
    ("avg_l2_inval", "L2D_CACHE_INVAL"),
    ("avg_l2_refill", "L2D_CACHE_REFILL"),
    ("avg_ll_miss_rd", "LL_CACHE_MISS_RD"),
    ("t2_jitter_us", "T_resp^jitter (us)"),
)
CONDITION_STYLES = {
    "oee_idle_rtos_idle": {
        "color": "#C8C8C8",
        "hatch": "",
    },
    "oee_yolov8n_rtos_idle": {
        "color": "#C8782A",
        "hatch": "//",
    },
    "oee_yolov8n_rtos_stress": {
        "color": "#994141",
        "hatch": "\\\\",
    },
}

FOLDER_RE = re.compile(
    r"^(?P<method>Native|WP|SP)_(?P<condition>"
    r"oee_(?:idle|yolov8n)_rtos_(?:idle|stress))_"
    r"(?P<hz>\d+)Hz$"
)


def setup_matplotlib() -> None:
    """Configure compact, publication-oriented Matplotlib defaults."""
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
            "font.size": 9,
            "axes.labelsize": 10,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )


def parse_summary(summary_path: Path) -> dict[str, str | float] | None:
    """Read t2_jitter from the last complete RTOS block of one run."""
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    status = str(summary.get("status") or "")
    if status not in {"complete", "rerun"}:
        return None

    folder_match = FOLDER_RE.fullmatch(summary_path.parent.name)
    if not folder_match:
        return None
    if int(folder_match.group("hz")) != FREQUENCY_HZ:
        return None

    expected_samples = int(summary.get("minimum_pmu_samples") or 0)
    prefix = summary_path.name.removesuffix("_summary.json")
    rtos_path = summary_path.with_name(f"{prefix}_rtos.log")
    if expected_samples <= 0 or not rtos_path.is_file():
        return None

    rtos_text = rtos_path.read_text(encoding="utf-8", errors="ignore")
    current_run = base.current_rtos_run(rtos_text)
    complete_block = base.last_complete_block(current_run, expected_samples)
    if complete_block is None:
        return None

    parsed_metrics: dict[str, float] = {}
    for pattern_name, record_name in (
        ("avg_l2_inval", "avg_l2_inval"),
        ("avg_l2_refill", "avg_l2_refill"),
        ("avg_ll_miss_rd", "avg_ll_miss_rd"),
        ("t2_jitter", "t2_jitter_us"),
    ):
        values = base.PATTERNS[pattern_name].findall(complete_block)
        if not values:
            return None
        parsed_metrics[record_name] = float(values[-1])

    return {
        "method": folder_match.group("method"),
        "condition": folder_match.group("condition"),
        **parsed_metrics,
        "started_at": str(summary.get("started_at") or ""),
        "status": status,
        "source": summary_path.relative_to(MATERIAL_ROOT).as_posix(),
    }


def collect_results() -> dict[tuple[str, str], dict[str, str | float]]:
    """Collect the newest completed result for every plotted bar."""
    latest: dict[tuple[str, str], dict[str, str | float]] = {}
    for summary_path in sorted(DATA_DIR.glob("*/*_summary.json")):
        record = parse_summary(summary_path)
        if record is None:
            continue
        key = (str(record["method"]), str(record["condition"]))
        previous = latest.get(key)
        if previous is None or str(record["started_at"]) > str(
            previous["started_at"]
        ):
            latest[key] = record

    missing = [
        (method, condition)
        for method in METHODS
        for condition in CONDITIONS
        if (method, condition) not in latest
    ]
    if missing:
        formatted = ", ".join(f"{method}/{condition}" for method, condition in missing)
        raise RuntimeError(f"Missing completed {FREQUENCY_HZ}Hz data: {formatted}")
    for record in latest.values():
        if record["status"] == "rerun":
            print(
                "[Warning] Using a fully sampled RTOS block marked for rerun: "
                f"{record['source']}"
            )
    return latest


def save_data(
    results: dict[tuple[str, str], dict[str, str | float]],
) -> Path:
    """Save the exact values plotted in the figure."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output = OUTPUT_DIR / "rk3588_yolov8n_tresp_jitter_motivation2_3_data.csv"
    with output.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "frequency_hz",
                "condition",
                "method",
                "avg_l2_inval",
                "avg_l2_refill",
                "avg_ll_miss_rd",
                "t2_jitter_us",
                "source",
            )
        )
        for condition, label in CONDITIONS.items():
            for method in METHODS:
                record = results[(method, condition)]
                writer.writerow(
                    (
                        FREQUENCY_HZ,
                        label,
                        method,
                        f"{float(record['avg_l2_inval']):.3f}",
                        f"{float(record['avg_l2_refill']):.3f}",
                        f"{float(record['avg_ll_miss_rd']):.3f}",
                        f"{float(record['t2_jitter_us']):.3f}",
                        record["source"],
                    )
                )
    return output


def plot_results(
    results: dict[tuple[str, str], dict[str, str | float]],
) -> tuple[Path, Path]:
    """Draw the three-condition grouped bar chart."""
    fig, ax = plt.subplots(figsize=(7.1, 3.6))
    x = np.arange(len(METHODS), dtype=float)
    width = 0.25
    all_values = []

    for condition_index, (condition, label) in enumerate(CONDITIONS.items()):
        values = [
            float(results[(method, condition)]["t2_jitter_us"])
            for method in METHODS
        ]
        all_values.extend(values)
        positions = x + (condition_index - 1) * width
        style = CONDITION_STYLES[condition]
        bars = ax.bar(
            positions,
            values,
            width=width,
            label=label,
            color=style["color"],
            edgecolor="black",
            linewidth=0.8,
            hatch=style["hatch"],
            zorder=3,
        )
        for bar, value in zip(bars, values):
            ax.annotate(
                f"{value:.2f}",
                xy=(bar.get_x() + bar.get_width() / 2, value),
                xytext=(0, 3),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=7.5,
            )

    if not all(math.isfinite(value) for value in all_values):
        raise RuntimeError("Non-finite t2_jitter value found")

    ax.set_xticks(x, [METHOD_LABELS[method] for method in METHODS])
    ax.set_xlabel("Method")
    ax.set_ylabel(
        r"Peak-to-peak response-time variation, "
        r"$J_{\mathrm{resp}}$ ($\mu\mathrm{s}$)"
    )
    ax.set_xlim(-0.62, len(METHODS) - 0.38)
    ax.set_ylim(0, max(all_values) * 1.18)
    ax.grid(
        axis="y",
        linestyle="--",
        linewidth=0.55,
        color="#B0B0B0",
        alpha=0.65,
        zorder=0,
    )
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)
    ax.legend(
        loc="lower center",
        bbox_to_anchor=(0.5, 1.01),
        ncol=3,
        frameon=False,
        handlelength=1.7,
        handletextpad=0.45,
        columnspacing=0.9,
    )
    fig.subplots_adjust(left=0.13, right=0.99, bottom=0.16, top=0.83)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / "rk3588_yolov8n_tresp_jitter_motivation2_3"
    png_path = basename.with_suffix(".png")
    pdf_path = basename.with_suffix(".pdf")
    fig.savefig(png_path, dpi=300, bbox_inches="tight")
    fig.savefig(pdf_path, bbox_inches="tight")
    plt.close(fig)
    return png_path, pdf_path


def print_metric_tables(
    results: dict[tuple[str, str], dict[str, str | float]],
) -> None:
    """Print PMU and response-time metrics as condition-specific tables."""
    metric_width = 22
    value_width = 12
    table_width = metric_width + value_width * len(METHODS)
    for condition, condition_label in CONDITIONS.items():
        print(f"\n{condition_label} at {FREQUENCY_HZ} Hz")
        print(
            f"{'Metric':<{metric_width}}"
            + "".join(f"{method:>{value_width}}" for method in METHODS)
        )
        print("-" * table_width)
        for metric_index, (metric_key, metric_label) in enumerate(
            PRINTED_METRICS
        ):
            if metric_index == len(PRINTED_METRICS) - 1:
                print("-" * table_width)
            values = [
                float(results[(method, condition)][metric_key])
                for method in METHODS
            ]
            print(
                f"{metric_label:<{metric_width}}"
                + "".join(f"{value:>{value_width}.5f}" for value in values)
            )


def main() -> int:
    setup_matplotlib()
    results = collect_results()

    print(f"\nT_resp jitter at {FREQUENCY_HZ} Hz (us)")
    print(
        f"{'Method':<8}"
        f"{'Idle':>10}"
        f"{'Inter-core':>14}"
        f"{'Inter+intra-core':>19}"
    )
    print("-" * 51)
    for method in METHODS:
        values = [
            float(results[(method, condition)]["t2_jitter_us"])
            for condition in CONDITIONS
        ]
        print(
            f"{method:<8}"
            f"{values[0]:>10.2f}"
            f"{values[1]:>14.2f}"
            f"{values[2]:>19.2f}"
        )
    print()
    print_metric_tables(results)
    print()

    png_path, pdf_path = plot_results(results)
    csv_path = save_data(results)
    print(f"[Saved] {png_path}")
    print(f"[Saved] {pdf_path}")
    print(f"[Saved] {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
