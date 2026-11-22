# xsv-cpp benchmark report

Generated from `results/runs/20261120T102645Z-formal/formal/timing.jsonl` and `results/runs/20261120T102645Z-formal/formal/memory.jsonl`. Every table cell is produced by `scripts/generate_report.py`; no value is typed by hand.

Variants present: rust, base, all.
Cases: 10. Timing records: 600. Memory records: 30.
Environment fingerprint: `eb48cb051d1d013f73cc5a76178b23a7ce90908b94ce1347e268910c3dbf3bc3` (13th Gen Intel(R) Core(TM) i5-13600K).

## 1. Wall time

Median wall-clock time per case, milliseconds. `ratio` is the candidate divided by the reference (lower is better); `<1` means faster.

| case | rust | base | all | base/rust | all/rust | all/base |
| --- | --- | --- | --- | --- | --- | --- |
| `B01.count.large` | 2956.454 | 8188.872 | 2223.219 | 2.770 | 0.752 | 0.271 |
| `B01.count.medium` | 182.446 | 486.274 | 156.385 | 2.665 | 0.857 | 0.322 |
| `B01.count.small` | 11.952 | 21.493 | 11.129 | 1.798 | 0.931 | 0.518 |
| `B02.index.medium` | 184.232 | 496.821 | 163.797 | 2.697 | 0.889 | 0.330 |
| `B03.select.medium` | 210.681 | 534.668 | 198.275 | 2.538 | 0.941 | 0.371 |
| `B04.search.medium` | 334.658 | 988.694 | 662.359 | 2.954 | 1.979 | 0.670 |
| `B05.sort.medium` | 603.625 | 1191.843 | 753.466 | 1.974 | 1.248 | 0.632 |
| `B06.join.medium` | 336.376 | 337.851 | 198.783 | 1.004 | 0.591 | 0.588 |
| `B07.stats.medium` | 808.213 | 1220.999 | 937.724 | 1.511 | 1.160 | 0.768 |
| `B10.slice.medium` | 3.662 | 297.044 | 2.473 | 81.123 | 0.675 | 0.008 |

## 2. Peak RSS

`/usr/bin/time -v` median across the configured runs. Values are MiB. Streaming `all` builds drop consumed mapped pages every 8 MiB; the window, not the file size, dominates their RSS.

| case | rust | base | all |
| --- | --- | --- | --- |
| `B01.count.large` | 5.0 | 4099.8 | 26.5 |
| `B01.count.medium` | 5.0 | 260.1 | 12.7 |
| `B01.count.small` | 5.0 | 12.2 | 11.9 |
| `B02.index.medium` | 5.0 | 260.1 | 209.1 |
| `B03.select.medium` | 5.0 | 260.2 | 12.8 |
| `B04.search.medium` | 5.0 | 260.4 | 13.6 |
| `B05.sort.medium` | 623.7 | 686.9 | 644.6 |
| `B06.join.medium` | 28.0 | 72.5 | 67.1 |
| `B07.stats.medium` | 5.1 | 260.2 | 12.7 |
| `B10.slice.medium` | 5.1 | 260.1 | 5.0 |

## 3. Throughput

Rows/s from the median wall time. Only cases with a dataset manifest have throughput.

| case | rust | base | all |
| --- | --- | --- | --- |
| `B01.count.large` | 3.38 M | 1.22 M | 4.50 M |
| `B01.count.medium` | 5.48 M | 2.06 M | 6.39 M |
| `B01.count.small` | 8.37 M | 4.65 M | 8.99 M |
| `B02.index.medium` | 5.43 M | 2.01 M | 6.11 M |
| `B03.select.medium` | 4.75 M | 1.87 M | 5.04 M |
| `B04.search.medium` | 2.99 M | 1.01 M | 1.51 M |
| `B05.sort.medium` | 1.66 M | 0.84 M | 1.33 M |
| `B06.join.medium` | 2.97 M | 2.96 M | 5.03 M |
| `B07.stats.medium` | 1.24 M | 0.82 M | 1.07 M |
| `B10.slice.medium` | 273.10 M | 3.37 M | 404.38 M |

## 4. Thread scaling

Median wall time for `stats -j N` on an indexed dataset. Speedup is relative to N=1.

| case | variant | threads | median ms | M rows/s | speedup |
| --- | --- | --- | --- | --- | --- |
| `B07.stats.indexed` | all | 1 | 1053.004 | 0.9496643781899782 | 1.000 |
| `B07.stats.indexed` | all | 2 | 573.814 | 1.7427260970023788 | 1.835 |
| `B07.stats.indexed` | all | 4 | 302.313 | 3.3078324810713466 | 3.483 |
| `B07.stats.indexed` | all | 8 | 205.186 | 4.873637913236914 | 5.132 |
| `B07.stats.indexed` | all | 16 | 127.183 | 7.862705989914507 | 8.279 |
| `B07.stats.indexed` | all | 32 | 121.572 | 8.225551837646442 | 8.662 |
| `B07.stats.indexed` | base | 1 | 1325.809 | 0.7542563268857416 | 1.000 |
| `B07.stats.indexed` | base | 2 | 1373.784 | 0.7279163975446904 | 0.965 |
| `B07.stats.indexed` | base | 4 | 1392.448 | 0.718159818440617 | 0.952 |
| `B07.stats.indexed` | base | 8 | 1402.622 | 0.7129505496346799 | 0.945 |
| `B07.stats.indexed` | base | 16 | 1402.865 | 0.7128271487498513 | 0.945 |
| `B07.stats.indexed` | base | 32 | 1415.097 | 0.7066651758569648 | 0.937 |
| `B07.stats.indexed` | rust | 1 | 966.251 | 1.0349275563600784 | 1.000 |
| `B07.stats.indexed` | rust | 2 | 495.683 | 2.017416807092119 | 1.949 |
| `B07.stats.indexed` | rust | 4 | 259.023 | 3.8606598751497403 | 3.730 |
| `B07.stats.indexed` | rust | 8 | 170.151 | 5.8771236312600585 | 5.679 |
| `B07.stats.indexed` | rust | 16 | 99.585 | 10.041626408648895 | 9.703 |
| `B07.stats.indexed` | rust | 32 | 104.117 | 9.604583339385627 | 9.280 |

## 5. Ablation

Speedup of each single-axis build over `base` (>1 means faster for the same case).

| case | pst | pmr | simd | tbb | hash | mmap |
| --- | --- | --- | --- | --- | --- | --- |
| `B03.select.medium` | 1.038 | 1.084 | 1.213 | 1.088 | 1.052 | 2.206 |
| `B05.sort.medium` | 1.117 | 1.021 | 1.071 | 1.161 | 1.027 | 1.321 |
| `B06.join.medium` | 1.029 | 1.070 | 1.058 | 1.076 | 1.269 | 1.230 |
| `B07.stats.medium` | 0.968 | 0.995 | 1.056 | 1.000 | 1.021 | 1.303 |
| `B10.slice.medium` | 0.922 | 1.168 | 1.159 | 1.145 | 1.142 | 125.136 |

## 6. Engineering complexity

| metric | value |
| --- | --- |
| C++ implementation LOC (headers included) | 5,634 |
| test LOC | 859 |
| documentation LOC | 1,218 |
| scripts LOC | 2,624 |
| direct dependencies | 5 |
| differential cases | 144 |
| CTest tests | 14 |
| gate pass rate | 100.0% |
| planned effort (person-days) | 60 |
| binary size (bytes) | 2,057,568 |
| incremental clean build (s) | 16.5 |


## 7. Interpretation guardrails

- `rust` is the unmodified 0.13.0 release; `base` is C++ with every optimization axis off; `all` is the optimized build.
- `all/rust` mixes language and algorithm. `all/base` isolates the optimization contribution; `base/rust` is the language-level number.
- A single dataset size cannot establish scaling; see the thread-scaling section once collected.
- These numbers are only valid on the machine whose fingerprint is in `results/runs/20261120T102645Z-formal/formal/environment.json`.
- `all` maps input files; peak RSS includes resident file-backed pages. Use the allocation counters (heaptrack or PMR statistics) to attribute allocator effects separately from page-cache residency.

