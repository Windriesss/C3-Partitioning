#!/usr/bin/env python3
"""Plot the five-allocation RK3568 translation-attribution data."""

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
DATA_ROOT = MATERIAL_ROOT / "data" / "rk3568_attribution"
RAW = DATA_ROOT / "raw" / "five_allocations"
PROCESSED = DATA_ROOT / "processed"
FIGURES = MATERIAL_ROOT / "figures" / "appendix" / "rk3568_attribution"
TABLES = MATERIAL_ROOT / "tables" / "rk3568_attribution"

PAPER = {
    "blue": "#4C72B0",
    "orange": "#DD8452",
    "red": "#B85C5C",
    "green": "#6A9F58",
    "purple": "#7A5195",
    "dark": "#3A3A3A",
    "gray": "#777777",
    "grid": "#E5E5E5",
}

CONDITIONS = {
    "same_candidate": {
        "label": "Same candidate", "color": PAPER["purple"],
        "linestyle": (0, (3, 2)), "marker": "o", "filled": False,
    },
    "same_baseline": {
        "label": "Same baseline", "color": PAPER["gray"],
        "linestyle": (0, (3, 2)), "marker": "o", "filled": False,
    },
    "cross_candidate": {
        "label": "Cross candidate", "color": PAPER["purple"],
        "linestyle": "-", "marker": "s", "filled": True,
    },
    "cross_baseline": {
        "label": "Cross baseline", "color": PAPER["gray"],
        "linestyle": "-", "marker": "s", "filled": True,
    },
}

METRICS = {
    "latency": {
        "source": "mean_ticks", "kind": "value",
        "label": "Mean probe latency (cycles)",
    },
    "l1d_refill": {
        "source": "probe_l1d_refill_samples", "kind": "incidence",
        "label": "Final reloads with L1D refill (%)",
    },
    "tlb_refill": {
        "source": "probe_l1d_tlb_refill_samples", "kind": "incidence",
        "label": "Final reloads with L1D-TLB refill (%)",
    },
    "ll_miss": {
        "source": "probe_ll_cache_miss_samples", "kind": "incidence",
        "label": "Final reloads with LL-cache miss (%)",
    },
}

EVENT_TOTALS = {
    "l1d_refill_events_per_window": "probe_l1d_refill_total",
    "tlb_refill_events_per_window": "probe_l1d_tlb_refill_total",
    "ll_miss_events_per_window": "probe_ll_cache_miss_total",
}


def configure_matplotlib():
    mpl.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 9.2,
        "axes.labelsize": 9.7,
        "xtick.labelsize": 8.2,
        "ytick.labelsize": 8.2,
        "legend.fontsize": 7.7,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
    })


def style_axis(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(PAPER["dark"])
    ax.spines["bottom"].set_color(PAPER["dark"])
    ax.tick_params(direction="out", length=3.1, width=0.8,
                   colors=PAPER["dark"])
    ax.grid(axis="y", color=PAPER["grid"], linewidth=0.7, zorder=0)


def read_result(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    metadata = {}
    for line in lines:
        if line.startswith("# ") and "," in line:
            key, value = line[2:].split(",", 1)
            metadata[key] = value
    rows = list(csv.DictReader(line for line in lines
                               if not line.startswith("#")))
    return metadata, rows


def run_name(path):
    return path.name.split("_cpu", 1)[0]


def validate(metadata, path):
    expected = {
        "schema_version": "18",
        "scan_order": "6",
        "probe_cpu": "3",
        "stimulus_cpus": "2",
        "addrmask": "0x7fffff",
        "candidate_target_masked": "0x38e900",
        "target_candidate_items": "16",
        "item_sweep_first": "0",
        "item_sweep_last": "20",
        "item_sweep_repetitions": "1000",
        "item_sweep_rounds": "1000",
        "scan_passes": "3",
        "attribution_cross_only_bits": "true",
        "probe_pmu_events_available": "true",
        "l1d_tlb_refill_event_available": "true",
        "ll_cache_events_available": "true",
    }
    for key, wanted in expected.items():
        if metadata.get(key) != wanted:
            raise SystemExit(
                f"{path.name}: expected {key}={wanted}, "
                f"got {metadata.get(key)!r}")


def row_metric(row, metric):
    spec = METRICS[metric]
    if spec["kind"] == "incidence":
        return 100.0 * int(row[spec["source"]]) / int(row["repetitions"])
    return float(row[spec["source"]])


def event_count(row, field):
    return float(row[field]) / int(row["repetitions"])


def summarize(values):
    values = list(values)
    return statistics.median(values), min(values), max(values)


def load_five_allocations():
    paths = sorted(RAW.glob("*_attribution.csv"))
    if len(paths) != 5:
        raise SystemExit(f"expected five attribution CSVs, got {len(paths)}")
    item = defaultdict(dict)
    for path in paths:
        metadata, rows = read_result(path)
        validate(metadata, path)
        run = run_name(path)
        for row in rows:
            if row["available"] != "1":
                continue
            if row["record"] == "item_sweep" and row["variant"] in CONDITIONS:
                item[(int(row["active_candidates"]), row["variant"])][run] = row
    return paths, item


def summarize_item(item):
    output = []
    for x in range(21):
        for condition in CONDITIONS:
            runs = item[(x, condition)]
            if len(runs) != 5:
                raise SystemExit(
                    f"item X={x} {condition}: expected 5 runs, got {len(runs)}")
            row = {"stimulus_items": x, "condition": condition,
                   "allocations": len(runs)}
            for metric in METRICS:
                center, low, high = summarize(
                    row_metric(cell, metric) for cell in runs.values())
                row[f"median_{metric}"] = center
                row[f"min_{metric}"] = low
                row[f"max_{metric}"] = high
            for name, field in EVENT_TOTALS.items():
                center, low, high = summarize(
                    event_count(cell, field) for cell in runs.values())
                row[f"median_{name}"] = center
                row[f"min_{name}"] = low
                row[f"max_{name}"] = high
            output.append(row)
    return output


def write_rows(path, rows):
    rows = list(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return path.resolve()


def save_figure(fig, stem):
    FIGURES.mkdir(parents=True, exist_ok=True)
    outputs = []
    for suffix in ("pdf", "svg", "png"):
        path = FIGURES / f"{stem}.{suffix}"
        kwargs = {"bbox_inches": "tight", "pad_inches": 0.03}
        if suffix == "png":
            kwargs["dpi"] = 450
        fig.savefig(path, **kwargs)
        outputs.append(path.resolve())
    plt.close(fig)
    return outputs


def draw_summary(ax, xs, rows, key_field, style, prefix=""):
    center = np.asarray([rows[x][f"median_{prefix}{key_field}"] for x in xs])
    low = np.asarray([rows[x][f"min_{prefix}{key_field}"] for x in xs])
    high = np.asarray([rows[x][f"max_{prefix}{key_field}"] for x in xs])
    ax.fill_between(xs, low, high, color=style["color"], alpha=0.06,
                    linewidth=0, zorder=1)
    ax.plot(xs, center, color=style["color"],
            linestyle=style["linestyle"], marker=style["marker"],
            markerfacecolor=style["color"] if style["filled"] else "white",
            markeredgecolor=style["color"], markeredgewidth=0.65,
            markersize=3.5, linewidth=1.5, label=style["label"], zorder=3)


def plot_all_conditions(item_summary):
    lookup = {(row["stimulus_items"], row["condition"]): row
              for row in item_summary}
    xs = list(range(21))
    fig, axes = plt.subplots(2, 2, figsize=(7.15, 5.15), sharex=True,
                             gridspec_kw={"hspace": 0.18, "wspace": 0.20})
    panel_specs = (
        ("latency", "(a) Probe latency"),
        ("l1d_refill", "(b) L1D refill incidence"),
        ("tlb_refill", "(c) L1D-TLB refill incidence"),
        ("ll_miss", "(d) LL-cache-miss incidence"),
    )
    for ax, (metric, title) in zip(axes.flat, panel_specs):
        style_axis(ax)
        for condition, style in CONDITIONS.items():
            rows = {x: lookup[(x, condition)] for x in xs}
            draw_summary(ax, xs, rows, metric, style)
        ax.set_title(title, loc="left", fontsize=9.2,
                     fontweight="normal", pad=4)
        ax.set_xlim(-0.4, 20.4)
        ax.set_xticks(np.arange(0, 21, 2))
        if metric == "latency":
            ax.set_ylim(15, 620)
            ax.set_ylabel("Cycles")
            legend_handles = [
                Line2D([0], [0], color=PAPER["purple"], linewidth=2.0,
                       label="Candidate group"),
                Line2D([0], [0], color=PAPER["gray"], linewidth=2.0,
                       label="Unrelated baseline"),
                Line2D([0], [0], color=PAPER["dark"],
                       linestyle=(0, (3, 2)), marker="o", markersize=4.2,
                       markerfacecolor="white", markeredgecolor=PAPER["dark"],
                       label="Same core"),
                Line2D([0], [0], color=PAPER["dark"], linestyle="-",
                       marker="s", markersize=4.2,
                       markerfacecolor=PAPER["dark"],
                       markeredgecolor=PAPER["dark"], label="Cross core"),
            ]
            ax.legend(handles=legend_handles, frameon=False, ncol=2,
                      loc="upper left", columnspacing=1.0,
                      handlelength=2.1)
        else:
            ax.set_ylim(-4, 104)
            ax.set_yticks([0, 25, 50, 75, 100])
            ax.set_ylabel("Reload windows (%)")
    for ax in axes[-1, :]:
        ax.set_xlabel("Number of stimulus addresses, X")
    fig.subplots_adjust(left=0.085, right=0.995, bottom=0.095, top=0.985)
    return save_figure(fig, "fig_a1_itemsweep_latency_and_pmu")


def key_item_rows(item_summary):
    wanted = {0, 3, 5, 6, 7, 11, 12, 13, 16, 20}
    return [row for row in item_summary if row["stimulus_items"] in wanted]


def write_x16_latex(path, rows):
    lookup = {row["condition"]: row for row in rows
              if row["stimulus_items"] == 16}
    lines = [
        r"\begin{table}[t]", r"\centering", r"\small",
        r"\caption{RK3568 final-reload metrics at $X=16$ across five fresh allocations.}",
        r"\label{tab:rk3568-x16-pmu}",
        r"\begin{tabular}{lrrrr}", r"\toprule",
        r"Condition & Cycles & L1D (\%) & TLB (\%) & LL miss (\%) \\",
        r"\midrule",
    ]
    for condition in CONDITIONS:
        row = lookup[condition]
        lines.append(
            f"{CONDITIONS[condition]['label']} & "
            f"{row['median_latency']:.0f} & "
            f"{row['median_l1d_refill']:.1f} & "
            f"{row['median_tlb_refill']:.1f} & "
            f"{row['median_ll_miss']:.1f} " + r"\\")
    lines.extend([r"\bottomrule", r"\end{tabular}", r"\end{table}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    return path.resolve()


def main():
    configure_matplotlib()
    paths, item = load_five_allocations()
    item_summary = summarize_item(item)

    outputs = [
        write_rows(PROCESSED / "itemsweep_all_conditions.csv",
                   item_summary),
        write_rows(TABLES / "table_itemsweep_key_points.csv",
                   key_item_rows(item_summary)),
        write_x16_latex(TABLES / "table_x16_conditions.tex",
                        item_summary),
    ]
    outputs.extend(plot_all_conditions(item_summary))

    print("Input allocations:")
    for path in paths:
        print(f"  {path.resolve()}")
    print("Generated files:")
    for path in outputs:
        print(f"  {path}")


if __name__ == "__main__":
    main()
