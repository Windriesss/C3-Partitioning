# C3-Partitioning Reproduction Material

Author: HuaijiGao

This package contains the datasets, plotting programs, experiment source-code
excerpts, figures, and tables supplied with the C3-Partitioning paper. All data
and script references are relative to this directory, so the package can be
moved to another machine without editing paths.

## Directory map

| Path | Contents |
|---|---|
| `code/` | Linux snoop-filter experiments, RK3568 attribution experiment, and RTOS workloads |
| `data/` | Main-paper, appendix, and supplementary datasets; see `data/README.md` |
| `scripts/` | Figure and table generation programs; see `scripts/README.md` |
| `figures/main/` | Ten main-paper figures and plot-ready CSV files |
| `figures/appendix/` | Five appendix figures and plot-ready CSV files |
| `figures/supplementary/` | RK3568 25-panel diagnostic figure and processed CSV |
| `tables/` | Generated appendix CSV and LaTeX tables |
| `DATA_INDEX.csv` | Machine-readable figure/table-to-script-to-data mapping |
| `generate_all.py` | Grouped generation and output verification entry point |
| `requirements.txt` | Python dependency constraints |
| `C3_Partitioning_TC_Appendix_v2.0.pdf` | Typeset paper appendix containing the five appendix figures and RK3568 attribution table |

The detailed data fields, units, platform records, and statistical methods are
documented in `data/README.md`. Source contents and build limitations are
documented in `code/README.md`. Every figure and table is mapped in
`DATA_INDEX.csv`; `scripts/README.md` gives the same mapping in readable form.

## Python environment

- Python 3.10 or later
- Matplotlib 3.8 or later
- NumPy 1.25 or later

Install the declared dependencies from the package root:

```bash
python -m pip install -r requirements.txt
```

Exact Xen, UniProton, Linux, compiler, and stress-ng versions are not
consistently preserved by all datasets and are therefore not inferred here.
Python and library versions beyond the stated minimums are not claimed to
produce bit-for-bit identical PDF files across operating systems.

## Generate outputs

Run all main-paper, appendix, and supplementary generators:

```bash
python generate_all.py
```

Or generate one output group:

```bash
python generate_all.py --group main
python generate_all.py --group appendix
python generate_all.py --group supplementary
```

Expected paper outputs are ten main-paper figures, five appendix figures, one
supplementary figure, and two appendix tables. Figures are produced as PDF and
PNG; the snoop-filter, attribution, and supplementary plotters also write SVG.
Plot-ready CSV files are written beside the figures or under `data/` as listed
in `DATA_INDEX.csv`.

## Source-code scope

The source package contains experiment-specific Linux kernel modules,
board-side scripts, offline analysis programs, and the FFT and Franka RTOS
workload excerpts. These files are not a complete, independently buildable
Xen-UniProton system. Rebuilding or running them requires the original Xen and
UniProton trees, target Linux kernel build tree, board-support and linker
configuration, cross-toolchain, deployment environment, and workload
integration that are not included in this archive.
