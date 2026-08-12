#!/usr/bin/env python3
"""Generate and verify the figures and tables in this material package."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent

GROUPS = {
    "main": (
        "scripts/response_time/main/plot_yolov8n_motivation1_fft.py",
        "scripts/response_time/main/plot_yolov8n_motivation2_3.py",
        "scripts/response_time/main/plot_jresp_1000hz_bootstrap_ci.py",
        "scripts/response_time/main/plot_tresp_ccdf_1000hz_16mb.py",
        "scripts/response_time/main/plot_3588_pmu_1000hz.py",
        "scripts/response_time/main/plot_ablation_sensitivity_3588.py",
        "scripts/snoop_filter/plot_cross_platform_low23_threefig.py",
    ),
    "appendix": (
        "scripts/appendix/rk3568_attribution/plot_appendix.py",
        "scripts/response_time/low_rate/plot_jresp_100_10hz_single_run.py",
        "scripts/response_time/low_rate/plot_ablation_sensitivity_3588.py",
    ),
    "supplementary": (
        "scripts/supplementary/rk3568_bit_item/plot_bit_item_25panel.py",
    ),
}

EXPECTED = {
    "main": tuple(
        f"figures/main/{stem}.{suffix}"
        for stem in (
            "rk3588_yolov8n_fft_1536kb_motivation",
            "rk3588_yolov8n_tresp_jitter_motivation2_3",
            "sf_item_sweep_rk3588_rk3568",
            "sf_single_bit_rk3588_rk3568",
            "sf_rk3588_pa17_16_color_matrix",
            "sf_rk3568_pa14_13_color_matrix",
            "rk3588_rk3568_jresp_1000Hz_2x2",
            "rk3588_rk3568_tresp_ccdf_1000Hz_16MiB_2x2",
            "rk3588_pmu_1000Hz_2x3",
            "rk3588_1000hz_ablation_sensitivity_tresp_1x4",
        )
        for suffix in ("pdf", "png")
    ),
    "appendix": tuple(
        f"figures/appendix/{subdir}/{stem}.{suffix}"
        for subdir, stem in (
            ("rk3568_attribution", "fig_a1_itemsweep_latency_and_pmu"),
            ("response_time", "rk3588_rk3568_jresp_100Hz_2x2"),
            ("response_time", "rk3588_rk3568_jresp_10Hz_2x2"),
            ("response_time", "rk3588_100Hz_ablation_sensitivity_tresp_1x4"),
            ("response_time", "rk3588_10Hz_ablation_sensitivity_tresp_1x4"),
        )
        for suffix in ("pdf", "png")
    ) + (
        "tables/rk3568_attribution/table_itemsweep_key_points.csv",
        "tables/rk3568_attribution/table_x16_conditions.tex",
    ),
    "supplementary": tuple(
        f"figures/supplementary/fig_s1_rk3568_bit_item_25panel.{suffix}"
        for suffix in ("pdf", "png")
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--group",
        choices=("all", *GROUPS),
        default="all",
        help="output group to generate (default: all)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    selected = tuple(GROUPS) if args.group == "all" else (args.group,)

    for group in selected:
        for relative in GROUPS[group]:
            print(f"\n=== {relative} ===", flush=True)
            subprocess.run(
                [sys.executable, str(ROOT / relative)], cwd=ROOT, check=True
            )

    expected = [item for group in selected for item in EXPECTED[group]]
    missing = [item for item in expected if not (ROOT / item).is_file()]
    if missing:
        raise RuntimeError("Missing expected outputs: " + ", ".join(missing))

    print(f"\nGenerated and verified {len(expected)} output files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
