"""Plot the RK3588 Motivation 1 FFT experiment with and without YOLOv8n."""

from __future__ import annotations

import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
DATA_DIR = MATERIAL_ROOT / "data" / "response_time" / "main" / "rk3588" / "result_yolov8n_FFT"
OUTPUT_DIR = MATERIAL_ROOT / "figures" / "main"

# The ordering is intentional and matches the order used in the paper.
METHODS = ("Native", "WP", "SP")
METHOD_LABELS = {
    "Native": "Baseline",
    "WP": "WP",
    "SP": "SP",
}
CONDITIONS = {
    "idle": "Idle",
    "yolov8n": "Inter-core interference",
}

WORKSET_KB = 1536
FFT_SIZE = 131072
FFT_AVG_RE = re.compile(
    rf"\[FFT\]\s+N=\s*{FFT_SIZE}\s*\|\s*"
    rf"workset=\s*{WORKSET_KB}\s*KB\s*\|\s*"
    r"avg=\s*(\d+)\s*us"
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


def data_path(method: str, condition: str) -> Path:
    """Return the raw FFT result file for one method and condition."""
    return DATA_DIR / method / f"{method}_{condition}"


def parse_1536kb_averages(path: Path) -> np.ndarray:
    """Read all per-round 1536-KB FFT average response times."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing FFT result: {path}")

    text = path.read_text(encoding="utf-8", errors="ignore")
    values = np.asarray([float(value) for value in FFT_AVG_RE.findall(text)])
    if values.size == 0:
        raise ValueError(
            f"No N={FFT_SIZE}, workset={WORKSET_KB} KB FFT samples in {path}"
        )
    return values


def collect_results() -> dict[str, dict[str, dict[str, float | int]]]:
    """Collect mean, standard deviation, and sample count for every bar."""
    results: dict[str, dict[str, dict[str, float | int]]] = {}
    for condition in CONDITIONS:
        results[condition] = {}
        for method in METHODS:
            samples = parse_1536kb_averages(data_path(method, condition))
            results[condition][method] = {
                "mean_us": float(np.mean(samples)),
                "std_us": float(np.std(samples, ddof=1))
                if samples.size > 1
                else 0.0,
                "samples": int(samples.size),
            }
    return results


def save_data(
    results: dict[str, dict[str, dict[str, float | int]]],
) -> Path:
    """Save the plotted statistics to a CSV file for reproducibility."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    output = OUTPUT_DIR / "rk3588_yolov8n_fft_1536kb_data.csv"
    with output.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            ("condition", "method", "mean_us", "std_us", "samples", "source")
        )
        for condition in CONDITIONS:
            for method in METHODS:
                stats = results[condition][method]
                writer.writerow(
                    (
                        condition,
                        method,
                        f"{stats['mean_us']:.3f}",
                        f"{stats['std_us']:.3f}",
                        stats["samples"],
                        data_path(method, condition).relative_to(MATERIAL_ROOT).as_posix(),
                    )
                )
    return output


def plot_results(
    results: dict[str, dict[str, dict[str, float | int]]],
) -> tuple[Path, Path]:
    """Draw one grouped motivation figure and save PNG and PDF versions."""
    fig, ax = plt.subplots(figsize=(6.6, 3.6))
    x = np.arange(len(METHODS))
    width = 0.34
    styles = {
        "idle": {
            "color": "#C8C8C8",
            "hatch": "",
        },
        "yolov8n": {
            "color": "#C8782A",
            "hatch": "//",
        },
    }
    all_means = []

    for condition_index, (condition, condition_label) in enumerate(
        CONDITIONS.items()
    ):
        means = [
            float(results[condition][method]["mean_us"])
            for method in METHODS
        ]
        all_means.extend(means)
        positions = x + (condition_index - 0.5) * width
        style = styles[condition]
        bars = ax.bar(
            positions,
            means,
            width=width,
            label=condition_label,
            color=style["color"],
            edgecolor="black",
            linewidth=0.8,
            hatch=style["hatch"],
            zorder=3,
        )

        for bar, value in zip(bars, means):
            ax.annotate(
                f"{value:.0f}",
                xy=(bar.get_x() + bar.get_width() / 2, value),
                xytext=(0, 3),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=7.5,
            )

    ax.set_xticks(x, [METHOD_LABELS[method] for method in METHODS])
    ax.set_xlabel("Method")
    ax.set_ylabel(r"Average FFT execution time ($\mu$s)")
    ax.set_xlim(-0.6, len(METHODS) - 0.4)
    ax.set_ylim(0, max(all_means) * 1.18)
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
        ncol=2,
        frameon=False,
        handlelength=1.7,
        handletextpad=0.5,
        columnspacing=1.1,
    )
    fig.subplots_adjust(left=0.14, right=0.985, bottom=0.16, top=0.84)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    basename = OUTPUT_DIR / "rk3588_yolov8n_fft_1536kb_motivation"
    png_path = basename.with_suffix(".png")
    pdf_path = basename.with_suffix(".pdf")
    fig.savefig(png_path, dpi=300, bbox_inches="tight")
    fig.savefig(pdf_path, bbox_inches="tight")
    plt.close(fig)
    return png_path, pdf_path


def main() -> int:
    setup_matplotlib()
    results = collect_results()

    for condition, label in CONDITIONS.items():
        for method in METHODS:
            stats = results[condition][method]
            print(
                f"[{label:7s}] {method:6s}: "
                f"{stats['mean_us']:.2f} us "
                f"(n={stats['samples']}, std={stats['std_us']:.2f} us)"
            )

    png_path, pdf_path = plot_results(results)
    csv_path = save_data(results)
    print(f"[Saved] {png_path}")
    print(f"[Saved] {pdf_path}")
    print(f"[Saved] {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
