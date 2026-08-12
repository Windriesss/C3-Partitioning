#!/usr/bin/env python3
"""Shared CSV, statistics, and Matplotlib helpers for cross-platform SF plots."""

from __future__ import annotations

import csv
import statistics
from pathlib import Path

import matplotlib as mpl
mpl.use("Agg")
import matplotlib.pyplot as plt


PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "red": "#B85C5C",
    "dark": "#3A3A3A",
    "gray": "#777777",
    "grid": "#E5E5E5",
    "pale": "#F2F2F2",
}


def configure_matplotlib():
    mpl.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 9.2,
        "axes.labelsize": 10,
        "xtick.labelsize": 8.3,
        "ytick.labelsize": 8.3,
        "legend.fontsize": 8.0,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
    })


def project_paths(script_path):
    material_root = Path(script_path).resolve().parents[2]
    root = material_root / "data" / "snoop_filter"
    return {
        "root": root,
        "rk3588": root / "rk3588" / "pa17_16_five_allocations",
        "rk3568": root / "rk3568" / "low23_threefig_five_allocations",
        "cross_output": material_root / "figures" / "main",
        "rk3588_output": material_root / "figures" / "main",
        "rk3568_output": material_root / "figures" / "main",
    }


def read_result(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines if not line.startswith("#")))
    return metadata, rows


def run_name(metadata):
    return metadata["experiment_run_id"].split("_cpu", 1)[0]


def median_range(values):
    values = list(values)
    if not values:
        raise ValueError("cannot summarize an empty measurement set")
    return statistics.median(values), min(values), max(values)


def write_rows(path: Path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return path.resolve()


def style_axis(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(PAPER["dark"])
    ax.spines["bottom"].set_color(PAPER["dark"])
    ax.tick_params(direction="out", length=3.2, width=0.8,
                   colors=PAPER["dark"])
    ax.grid(axis="y", color=PAPER["grid"], linewidth=0.7, zorder=0)


def save_figure(fig, output_dir: Path, stem: str, *, tight=True):
    output_dir.mkdir(parents=True, exist_ok=True)
    paths = [output_dir.resolve() / f"{stem}.{suffix}"
             for suffix in ("pdf", "svg", "png")]
    kwargs = {"bbox_inches": "tight", "pad_inches": 0.03} if tight else {}
    fig.savefig(paths[0], **kwargs)
    fig.savefig(paths[1], **kwargs)
    fig.savefig(paths[2], dpi=450, **kwargs)
    plt.close(fig)
    return paths


def print_outputs(paths):
    print("Generated files:")
    for path in paths:
        print(f"  {Path(path).resolve()}")
