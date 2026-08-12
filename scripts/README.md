# Generation Scripts

Author: HuaijiGao

Run these programs with Python 3.10 or later after installing the package-level
`requirements.txt`. Each program resolves input and output paths from its own
location; the current working directory does not affect dataset lookup.

## Commands and outputs

| Command | Input data | Generated outputs |
|---|---|---|
| `python scripts/response_time/main/plot_yolov8n_motivation1_fft.py` | `data/response_time/main/rk3588/result_yolov8n_FFT/` | `figures/main/rk3588_yolov8n_fft_1536kb_motivation.*` and `_data.csv` |
| `python scripts/response_time/main/plot_yolov8n_motivation2_3.py` | `data/response_time/main/rk3588/result_yolov8n/` | `figures/main/rk3588_yolov8n_tresp_jitter_motivation2_3.*` and `_data.csv` |
| `python scripts/snoop_filter/plot_cross_platform_low23_threefig.py` | `data/snoop_filter/` | Four `sf_*` figure stems and `_data.csv` files under `figures/main/` |
| `python scripts/response_time/main/plot_jresp_1000hz_bootstrap_ci.py` | Both platform trees under `data/response_time/main/` | `figures/main/rk3588_rk3568_jresp_1000Hz_2x2.*` and bootstrap CSV |
| `python scripts/response_time/main/plot_tresp_ccdf_1000hz_16mb.py` | Both main response-time trees | `figures/main/rk3588_rk3568_tresp_ccdf_1000Hz_16MiB_2x2.*`, data CSV, and issues CSV |
| `python scripts/response_time/main/plot_3588_pmu_1000hz.py` | RK3588 main response-time data | `figures/main/rk3588_pmu_1000Hz_2x3.*` |
| `python scripts/response_time/main/plot_ablation_sensitivity_3588.py` | RK3588 main response-time data | `figures/main/rk3588_1000hz_ablation_sensitivity_tresp_1x4.*` and values CSV |
| `python scripts/appendix/rk3568_attribution/plot_appendix.py` | `data/rk3568_attribution/raw/five_allocations/` | Attribution figure, processed CSV, and both files under `tables/rk3568_attribution/` |
| `python scripts/response_time/low_rate/plot_jresp_100_10hz_single_run.py` | Both low-rate response-time trees | 100 Hz and 10 Hz Jresp figures and single-run CSV files |
| `python scripts/response_time/low_rate/plot_ablation_sensitivity_3588.py` | RK3588 low-rate response-time data | 100 Hz and 10 Hz ablation figures and combined values CSV |
| `python scripts/supplementary/rk3568_bit_item/plot_bit_item_25panel.py` | Supplementary RK3568 CSV | `figures/supplementary/fig_s1_rk3568_bit_item_25panel.*` and processed CSV |

Here `.*` means PDF and PNG; snoop-filter, attribution, and supplementary
scripts also produce SVG. The complete one-row-per-artifact mapping, including
statistics, is in `../DATA_INDEX.csv`.

To generate and verify grouped outputs from the package root, use:

```bash
python generate_all.py --group main
python generate_all.py --group appendix
python generate_all.py --group supplementary
```
