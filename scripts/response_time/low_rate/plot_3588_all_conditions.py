"""Plot the included RK3588 response-time conditions by OEE working set."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from fnmatch import fnmatchcase
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
MATERIAL_ROOT = SCRIPT_DIR.parents[2]
EVALUATION_ROOT = MATERIAL_ROOT / "data" / "response_time" / "low_rate"
DEFAULT_RESULTS_DIR = EVALUATION_ROOT / "rk3588" / "results"
DEFAULT_OUTPUT_DIR = SCRIPT_DIR / "output_3588"


def relative_source_path(path: Path) -> str:
    """Return a portable path relative to the low-rate data directory."""
    return path.resolve().relative_to(EVALUATION_ROOT.resolve()).as_posix()


VM_BYTES_ORDER = [
    "128K", "256K", "512K", "1M", "2M", "3M", "4M", "5M",
    "6M", "7M", "8M", "16M", "32M", "64M", "128M", "256M",
    "512M", "1G",
]

METRICS = {

    "t2_max": r"$T_{\mathrm{resp}}^{\max}$ ($\mu$s)",
    "t2_jitter": r"End-to-end jitter, $T_2^{jitter}$ ($\mu$s)",
    "avg_ll_miss_rd": "Avg. LL cache read misses",
    "avg_l2_refill": "Avg. L2D cache refills",
    "avg_l2_inval": "Avg. L2D cache invalidations",
    "oee_bogo_ops": "OEE stress-ng bogo ops",
    "rtos_copy_bogo_ops": "RTOS copy bogo ops (total rounds)",
}

PATTERNS = {
    "t1_max": re.compile(r"t1_max:\s*([\d.]+)\s*us"),
    "t2_max": re.compile(r"t2_max:\s*([\d.]+)\s*us"),
    "t2_jitter": re.compile(r"t2_jitter:\s*([\d.]+)\s*us"),
    "avg_ll_miss_rd": re.compile(r"AVG DIFF LL_CACHE_MISS_RD:\s*([\d.]+)"),
    "avg_l2_refill": re.compile(r"AVG DIFF L2D_CACHE_REFILL:\s*([\d.]+)"),
    "avg_l2_inval": re.compile(r"AVG DIFF L2D_CACHE_INVAL:\s*([\d.]+)"),
    "rtos_copy_bogo_ops": re.compile(
        r"copy rounds total/window:\s*(\d+)\s*/\s*\d+"
    ),
}

PMU_SAMPLES_RE = re.compile(r"PMU diff samples:\s*(\d+)")
RTOS_START_RE = re.compile(r"Hello,\s*QSemOS-RT", re.IGNORECASE)
OEE_BOGO_PATTERNS = [
    re.compile(r"bogo ops\s*[:=]\s*([\d.]+)", re.IGNORECASE),
    re.compile(
        r"(?im)^\s*stress-ng:\s*(?:info|metrc|metrics):\s*\[\d+\]\s+"
        r"vm\s+([\d.]+)\b"
    ),
]


def parse_hz_selector(value: str) -> int:
    match = re.fullmatch(r"(\d+)(?:Hz)?", value.strip(), re.IGNORECASE)
    if not match or int(match.group(1)) <= 0:
        raise argparse.ArgumentTypeError(
            f"Invalid frequency '{value}'; use values such as 10, 100, or 1000Hz"
        )
    return int(match.group(1))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument(
        "--hz",
        nargs="+",
        type=parse_hz_selector,
        help="Only plot these frequencies, for example: --hz 100 1000Hz",
    )
    parser.add_argument(
        "--condition",
        nargs="+",
        help=(
            "Only plot matching conditions/families, for example: "
            "--condition Native HCP_2-8; shell wildcards are also accepted"
        ),
    )
    parser.add_argument(
        "--scenario",
        nargs="+",
        choices=("idle", "stress"),
        help="Only plot idle, stress, or both RTOS scenarios",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Explicitly plot all data (this is also the default)",
    )
    args = parser.parse_args()
    if args.all and any((args.hz, args.condition, args.scenario)):
        parser.error("--all cannot be combined with --hz, --condition, or --scenario")
    return args


def setup_matplotlib() -> None:
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "DejaVu Serif"],
        "font.size": 9,
        "axes.labelsize": 10,
        "axes.titlesize": 10,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "legend.fontsize": 8,
        "axes.linewidth": 0.8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })


def vm_to_bytes(label: str) -> int:
    match = re.fullmatch(r"(\d+)([KMG])", label, re.IGNORECASE)
    if not match:
        raise ValueError(f"Unsupported VM bytes label: {label}")
    multipliers = {"K": 1024, "M": 1024**2, "G": 1024**3}
    return int(match.group(1)) * multipliers[match.group(2).upper()]


def vm_x(label: str) -> float:
    return math.log2(vm_to_bytes(label))


def display_tick(label: str) -> str:
    return "" if label in {"3M", "5M", "6M", "7M"} else label


def condition_label(condition: str) -> str:
    label = re.sub(r"_\d+Hz$", "", condition, flags=re.IGNORECASE)
    label = label.replace("_rtos_idle", " / Inter-core interference")
    label = label.replace("_rtos_stress", " / Inter-core + intra-core interference")
    return label.replace("_", " ")


def condition_family(condition: str) -> str:
    return re.split(r"_rtos_(?:idle|stress)", condition, maxsplit=1)[0]


def condition_scenario(condition: str) -> str:
    return "stress" if "_rtos_stress" in condition.lower() else "idle"


def condition_frequency_hz(condition: str) -> int | None:
    match = re.search(r"_(\d+)Hz$", condition, re.IGNORECASE)
    return int(match.group(1)) if match else None


def condition_matches(condition: str, selectors: list[str] | None) -> bool:
    if not selectors:
        return True
    candidates = (condition.lower(), condition_family(condition).lower())
    for raw_selector in selectors:
        selector = raw_selector.lower()
        if any(char in selector for char in "*?["):
            if any(fnmatchcase(candidate, selector) for candidate in candidates):
                return True
        elif any(
            candidate == selector or candidate.startswith(selector + "_")
            for candidate in candidates
        ):
            return True
    return False


def filter_records(records: list[dict], args: argparse.Namespace) -> list[dict]:
    if args.all:
        return list(records)
    frequencies = set(args.hz or [])
    scenarios = set(args.scenario or [])
    return [
        record
        for record in records
        if (not frequencies or condition_frequency_hz(record["condition"]) in frequencies)
        and condition_matches(record["condition"], args.condition)
        and (not scenarios or condition_scenario(record["condition"]) in scenarios)
    ]


def filter_description(args: argparse.Namespace) -> str:
    if args.all or not any((args.hz, args.condition, args.scenario)):
        return "all"
    parts = []
    if args.hz:
        parts.append("Hz=" + ",".join(str(value) for value in args.hz))
    if args.condition:
        parts.append("condition=" + ",".join(args.condition))
    if args.scenario:
        parts.append("scenario=" + ",".join(args.scenario))
    return "; ".join(parts)


def current_rtos_run(text: str) -> str:
    """Discard UART residue captured before the latest RTOS boot."""
    starts = list(RTOS_START_RE.finditer(text))
    if not starts:
        return text
    return text[starts[-1].start():]


def last_complete_block(text: str, expected_samples: int) -> str | None:
    blocks = text.split("===========================================================")
    for block in reversed(blocks):
        samples = PMU_SAMPLES_RE.findall(block)
        if not samples or int(samples[-1]) < expected_samples:
            continue
        if PATTERNS["t1_max"].search(block) and PATTERNS["t2_max"].search(block):
            return block
    return None


def parse_oee_bogos(summary: dict, oee_path: Path) -> float:
    value = summary.get("bogo_ops")
    if value is not None:
        return float(value)
    if not oee_path.is_file():
        return math.nan
    text = oee_path.read_text(encoding="utf-8", errors="ignore")
    for pattern in OEE_BOGO_PATTERNS:
        values = pattern.findall(text)
        if values:
            return float(values[-1])
    return math.nan


def parse_run(summary_path: Path, condition: str, vmbytes: str) -> tuple[dict | None, str | None]:
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, f"cannot read summary: {exc}"

    if summary.get("status") != "complete":
        return None, f"summary status={summary.get('status')}: {summary.get('error')}"

    expected_samples = int(summary.get("minimum_pmu_samples") or 0)
    if expected_samples <= 0:
        return None, "summary has no positive minimum_pmu_samples"

    prefix = summary_path.name.removesuffix("_summary.json")
    rtos_path = summary_path.with_name(f"{prefix}_rtos.log")
    oee_path = summary_path.with_name(f"{prefix}_oee.log")
    if not rtos_path.is_file():
        return None, f"missing RTOS log: {rtos_path.name}"

    text = rtos_path.read_text(encoding="utf-8", errors="ignore")
    block = last_complete_block(text, expected_samples)
    text = current_rtos_run(text)
    if block is None:
        return None, f"no complete RTOS block with >= {expected_samples} samples"

    metrics = {}
    missing = []
    for key, pattern in PATTERNS.items():
        values = pattern.findall(block)
        if values:
            metrics[key] = float(values[-1])
        else:
            metrics[key] = math.nan
            missing.append(key)

    # An idle RTOS legitimately reports zero copy rounds. A missing field is not zero.
    if "rtos_copy_bogo_ops" in missing:
        return None, "complete block is missing RTOS copy rounds"

    metrics["oee_bogo_ops"] = parse_oee_bogos(summary, oee_path)
    return {
        "condition": condition,
        "vmbytes": vmbytes,
        "summary": relative_source_path(summary_path),
        "rtos_log": relative_source_path(rtos_path),
        "oee_log": relative_source_path(oee_path),
        **metrics,
    }, None


def scan_records(results_dir: Path) -> tuple[list[dict], list[dict]]:
    results_dir = results_dir.resolve()
    evaluation_root = EVALUATION_ROOT.resolve()
    if not results_dir.is_relative_to(evaluation_root):
        raise RuntimeError(
            f"Plot data must stay inside the low-rate response-time data: {results_dir}"
        )
    records = []
    issues = []
    for condition_dir in sorted(path for path in results_dir.iterdir() if path.is_dir()):
        condition = condition_dir.name
        for vm_dir in sorted(path for path in condition_dir.iterdir() if path.is_dir()):
            if not vm_dir.name.startswith("vm_"):
                continue
            vmbytes = vm_dir.name.removeprefix("vm_")
            try:
                vm_to_bytes(vmbytes)
            except ValueError as exc:
                issues.append({"condition": condition, "vmbytes": vmbytes, "file": "-", "reason": str(exc)})
                continue
            for summary_path in sorted(vm_dir.glob("*_summary.json")):
                record, reason = parse_run(summary_path, condition, vmbytes)
                if reason:
                    issues.append({
                        "condition": condition,
                        "vmbytes": vmbytes,
                    "file": relative_source_path(summary_path),
                        "reason": reason,
                    })
                else:
                    records.append(record)
    return records, issues


def aggregate(records: list[dict]) -> list[dict]:
    grouped: dict[tuple[str, str, str], list[float]] = defaultdict(list)
    for record in records:
        for metric in METRICS:
            value = float(record.get(metric, math.nan))
            if math.isfinite(value):
                grouped[(record["condition"], record["vmbytes"], metric)].append(value)

    stats = []
    for (condition, vmbytes, metric), values in grouped.items():
        array = np.asarray(values, dtype=float)
        stats.append({
            "condition": condition,
            "vmbytes": vmbytes,
            "metric": metric,
            "n": len(array),
            "median": float(np.median(array)),
            "min": float(np.min(array)),
            "max": float(np.max(array)),
        })
    return stats


def make_styles(conditions: list[str]) -> dict[str, dict]:
    families = sorted({condition_family(condition) for condition in conditions})
    cmap = plt.get_cmap("tab20")
    colors = {family: cmap(index % 20) for index, family in enumerate(families)}
    markers = ["o", "s", "^", "D", "v", "P", "X", "<", ">", "h"]
    family_marker = {
        family: markers[index % len(markers)] for index, family in enumerate(families)
    }
    styles = {}
    for condition in conditions:
        scenario = condition_scenario(condition)
        family = condition_family(condition)
        styles[condition] = {
            "color": colors[family],
            "marker": family_marker[family],
            "linestyle": "--" if scenario == "stress" else "-",
            "markerfacecolor": colors[family] if scenario == "stress" else "white",
        }
    return styles


def available_vm_labels(stats: list[dict]) -> list[str]:
    labels = {item["vmbytes"] for item in stats}
    ordered = [label for label in VM_BYTES_ORDER if label in labels]
    extras = sorted(labels - set(ordered), key=vm_to_bytes)
    return ordered + extras


def plot_metric(ax, stats: list[dict], metric: str, styles: dict[str, dict], vm_labels: list[str]) -> None:
    by_key = {
        (item["condition"], item["vmbytes"]): item
        for item in stats
        if item["metric"] == metric
    }
    for condition in sorted(styles):
        labels = [label for label in vm_labels if (condition, label) in by_key]
        if not labels:
            continue
        points = [by_key[(condition, label)] for label in labels]
        x_values = np.asarray([vm_x(label) for label in labels])
        medians = np.asarray([point["median"] for point in points])
        minima = np.asarray([point["min"] for point in points])
        maxima = np.asarray([point["max"] for point in points])
        style = styles[condition]
        if any(point["n"] > 1 for point in points):
            ax.fill_between(x_values, minima, maxima, color=style["color"], alpha=0.12, linewidth=0)
        ax.plot(
            x_values,
            medians,
            label=condition_label(condition),
            color=style["color"],
            marker=style["marker"],
            markerfacecolor=style["markerfacecolor"],
            markeredgecolor=style["color"],
            linestyle=style["linestyle"],
            linewidth=1.45,
            markersize=4.0,
        )

    ax.set_xticks([vm_x(label) for label in vm_labels])
    ax.set_xticklabels([display_tick(label) for label in vm_labels], rotation=45, ha="right")
    ax.set_xlabel("OEE stress-ng --vm-bytes")
    ax.set_ylabel(METRICS[metric])
    ax.grid(axis="y", linestyle="--", linewidth=0.45, alpha=0.35)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)
    ax.ticklabel_format(style="plain", axis="y")


def save_figure(fig, output_dir: Path, basename: str) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_dir / f"{basename}.png", dpi=300, bbox_inches="tight")
    fig.savefig(output_dir / f"{basename}.pdf", bbox_inches="tight")
    print(f"[Saved] {output_dir / (basename + '.png')}")
    print(f"[Saved] {output_dir / (basename + '.pdf')}")


def plot_individual(stats: list[dict], styles: dict[str, dict], vm_labels: list[str], output_dir: Path) -> None:
    for metric in METRICS:
        fig, ax = plt.subplots(figsize=(9.0, 4.8))
        plot_metric(ax, stats, metric, styles, vm_labels)
        ax.set_title(f"RK3588: {METRICS[metric]}")
        handles, labels = ax.get_legend_handles_labels()
        if handles:
            ax.legend(handles, labels, loc="upper left", bbox_to_anchor=(1.02, 1.0), frameon=False)
        fig.tight_layout()
        save_figure(fig, output_dir, f"rk3588_{metric}_all_conditions")
        plt.close(fig)


def plot_overview(stats: list[dict], styles: dict[str, dict], vm_labels: list[str], output_dir: Path) -> None:
    fig, axes = plt.subplots(2, 4, figsize=(14.2, 7.1))
    axes_flat = axes.ravel()
    for ax, metric in zip(axes_flat, METRICS):
        plot_metric(ax, stats, metric, styles, vm_labels)
        ax.set_title(METRICS[metric])
    axes_flat[-1].axis("off")
    handles, labels = axes_flat[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="lower center", ncol=min(4, len(labels)), frameon=False)
    fig.tight_layout(rect=[0, 0.13, 1, 1])
    save_figure(fig, output_dir, "rk3588_all_metrics_all_conditions_overview")
    plt.close(fig)


def save_csv(records: list[dict], stats: list[dict], issues: list[dict], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_fields = ["condition", "vmbytes", *METRICS, "summary", "rtos_log", "oee_log"]
    with (output_dir / "rk3588_plot_raw_records.csv").open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=raw_fields)
        writer.writeheader()
        writer.writerows({field: record.get(field, "") for field in raw_fields} for record in records)
    with (output_dir / "rk3588_plot_statistics.csv").open("w", encoding="utf-8-sig", newline="") as output:
        fields = ["condition", "vmbytes", "metric", "n", "median", "min", "max"]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(stats)
    with (output_dir / "rk3588_plot_issues.csv").open("w", encoding="utf-8-sig", newline="") as output:
        fields = ["condition", "vmbytes", "file", "reason"]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(issues)


def main() -> int:
    args = parse_args()
    results_dir = args.results_dir.resolve()
    output_dir = args.output_dir.resolve()
    if not results_dir.is_dir():
        raise RuntimeError(f"RK3588 results directory does not exist: {results_dir}")

    setup_matplotlib()
    records, issues = scan_records(results_dir)
    if not records:
        raise RuntimeError("No complete RK3588 response-time records were found")
    stats = aggregate(records)
    conditions = sorted({record["condition"] for record in records})
    vm_labels = available_vm_labels(stats)
    styles = make_styles(conditions)

    print(
        f"[Loaded] complete_runs={len(records)}, conditions={len(conditions)}, "
        f"vm_points={len(vm_labels)}, excluded_issues={len(issues)}"
    )
    for condition in conditions:
        count = sum(record["condition"] == condition for record in records)
        print(f"[Condition] {condition}: {count} complete run(s)")

    save_csv(records, stats, issues, output_dir)
    plot_individual(stats, styles, vm_labels, output_dir)
    plot_overview(stats, styles, vm_labels, output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
