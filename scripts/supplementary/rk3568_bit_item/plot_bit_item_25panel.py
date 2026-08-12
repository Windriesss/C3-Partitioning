#!/usr/bin/env python3
"""Generate the RK3568 PA[0:24] bit-by-item 5x5 overview."""

from __future__ import annotations

import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib as mpl
mpl.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
INPUT = MATERIAL_ROOT / "data" / "supplementary" / "rk3568_bit_item" / "rk3568_bit_item_run01.csv"
OUTPUT = MATERIAL_ROOT / "figures" / "supplementary"
STEM = "fig_s1_rk3568_bit_item_25panel"

PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "dark": "#3A3A3A",
    "gray": "#777777",
    "grid": "#E5E5E5",
    "pale": "#F2F2F2",
}
VARIANTS = (
    "same_original_matching",
    "same_probe_only_flipped",
    "cross_original_matching",
    "cross_probe_only_flipped",
)
STYLE = {
    "same_original_matching": {
        "label": "Same, original", "color": PAPER["blue"],
        "linestyle": "-", "marker": "o", "filled": True,
    },
    "same_probe_only_flipped": {
        "label": "Same, probe flipped", "color": PAPER["blue"],
        "linestyle": (0, (3, 2)), "marker": "o", "filled": False,
    },
    "cross_original_matching": {
        "label": "Cross, original", "color": PAPER["orange"],
        "linestyle": "-", "marker": "s", "filled": True,
    },
    "cross_probe_only_flipped": {
        "label": "Cross, probe flipped", "color": PAPER["orange"],
        "linestyle": (0, (3, 2)), "marker": "s", "filled": False,
    },
}


def configure_matplotlib() -> None:
    mpl.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 8.5,
        "axes.labelsize": 8.7,
        "xtick.labelsize": 7.2,
        "ytick.labelsize": 7.2,
        "legend.fontsize": 8.3,
        "axes.linewidth": 0.75,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
    })


def read_result(path: Path) -> tuple[dict[str, str], list[dict[str, str]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def summarize(path: Path):
    metadata, rows = read_result(path)
    grouped = defaultdict(list)
    unavailable = {}
    for row in rows:
        if row["record"] != "bit_item_sweep":
            continue
        bit = int(row["test_bit"])
        if row["available"] != "1":
            unavailable[bit] = row["lookup_status"]
            continue
        grouped[(bit, int(row["active_candidates"]), row["variant"])].append(row)

    summary = {}
    output_rows = []
    for (bit, x, variant), cells in sorted(grouped.items()):
        if len(cells) != 5:
            raise RuntimeError(
                f"PA[{bit}] X={x} {variant}: expected five passes, got {len(cells)}"
            )
        samples = [float(cell["mean_ticks"]) for cell in cells]
        values = {
            "median_mean_cycles": statistics.median(samples),
            "min_mean_cycles": min(samples),
            "max_mean_cycles": max(samples),
            "passes": len(cells),
        }
        summary[(bit, x, variant)] = values
        output_rows.append({
            "bit": bit,
            "stimulus_items": x,
            "variant": variant,
            **values,
        })
    return metadata, summary, unavailable, output_rows


def write_rows(rows: list[dict]) -> Path:
    output = OUTPUT / f"{STEM}_data.csv"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return output


def style_axis(ax: plt.Axes) -> None:
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.grid(axis="y", color=PAPER["grid"], linewidth=0.45)
    ax.tick_params(direction="out", length=2.8, width=0.7)


def series(summary, bit: int, variant: str, xs: list[int]):
    cells = [summary[(bit, x, variant)] for x in xs]
    return (
        np.asarray([cell["median_mean_cycles"] for cell in cells]),
        np.asarray([cell["min_mean_cycles"] for cell in cells]),
        np.asarray([cell["max_mean_cycles"] for cell in cells]),
    )


def plot(summary, unavailable) -> list[Path]:
    fig, axes = plt.subplots(
        5, 5, figsize=(11.2, 9.0), sharex=True, sharey=True,
        gridspec_kw={"hspace": 0.28, "wspace": 0.15},
    )
    xs = list(range(21))
    for bit, ax in enumerate(axes.flat):
        style_axis(ax)
        if bit in unavailable:
            ax.set_facecolor(PAPER["pale"])
            ax.text(
                0.5, 0.52, "Flipped owned\npage unavailable",
                transform=ax.transAxes, ha="center", va="center",
                color=PAPER["gray"], fontsize=8,
            )
            ax.set_title(f"PA[{bit}]", fontsize=9, pad=2.5, color=PAPER["gray"])
            continue
        for variant in VARIANTS:
            spec = STYLE[variant]
            center, low, high = series(summary, bit, variant, xs)
            ax.fill_between(
                xs, low, high, color=spec["color"], alpha=0.055,
                linewidth=0, zorder=1,
            )
            ax.plot(
                xs, center, color=spec["color"],
                linestyle=spec["linestyle"], marker=spec["marker"],
                markerfacecolor=spec["color"] if spec["filled"] else "white",
                markeredgecolor=spec["color"], markeredgewidth=0.55,
                markersize=2.35, linewidth=1.0, zorder=3,
            )
        ax.set_title(f"PA[{bit}]", fontsize=9, pad=2.5)
        ax.set_xlim(-0.4, 20.4)
        ax.set_ylim(15, 620)
        ax.set_xticks([0, 5, 10, 15, 20])
        ax.set_yticks([0, 200, 400, 600])

    for row in range(5):
        axes[row, 0].set_ylabel("Cycles")
    for column in range(5):
        axes[-1, column].set_xlabel("Stimulus X")

    handles = [
        Line2D(
            [0], [0], color=spec["color"], linestyle=spec["linestyle"],
            marker=spec["marker"], markersize=4,
            markerfacecolor=spec["color"] if spec["filled"] else "white",
            markeredgecolor=spec["color"], label=spec["label"],
        )
        for spec in (STYLE[variant] for variant in VARIANTS)
    ]
    fig.legend(
        handles=handles, frameon=False, ncol=4, loc="upper center",
        bbox_to_anchor=(0.5, 0.995), columnspacing=1.4, handlelength=2.5,
    )
    fig.subplots_adjust(left=0.065, right=0.995, bottom=0.065, top=0.95)

    OUTPUT.mkdir(parents=True, exist_ok=True)
    outputs = []
    for suffix in ("pdf", "png", "svg"):
        output = OUTPUT / f"{STEM}.{suffix}"
        kwargs = {"bbox_inches": "tight", "pad_inches": 0.03}
        if suffix == "png":
            kwargs["dpi"] = 450
        fig.savefig(output, **kwargs)
        outputs.append(output)
    plt.close(fig)
    return outputs


def main() -> int:
    configure_matplotlib()
    metadata, summary, unavailable, rows = summarize(INPUT)
    expected_bits = set(range(int(metadata["bit_first"]), int(metadata["bit_last"]) + 1))
    measured_bits = {key[0] for key in summary}
    if measured_bits | set(unavailable) != expected_bits:
        raise RuntimeError("Available and unavailable records do not cover PA[0:24]")
    outputs = [write_rows(rows), *plot(summary, unavailable)]
    for output in outputs:
        print(f"[Saved] {output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
