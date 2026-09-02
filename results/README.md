# Measurement data

Raw samples and figures behind the tables in `docs/tabelas-heuristicas.tex`.
Every file here was produced by a command listed below, on the hardware
described below. Nothing was edited by hand.

## Platform

| | |
|---|---|
| Board | Raspberry Pi 5 (Cortex-A76, 4 cores) |
| Kernel | Linux with `PREEMPT_RT` |
| Scheduling | `SCHED_FIFO`, obtained by running the binary as root |
| CPU governor | `performance` |
| Compiler flags | as fixed in the `Makefile`; the benchmark objects **must** stay at `-O0`, or every `wcet_ns` in the deployment plans becomes wrong |
| Run length | 50 hyperperiods (`example_eval` second argument) |
| Cores used | 3, as declared in the `allocation` block of each plan |

## CSV format

One row per job, written by `examples/example_eval.cpp`:

```
subtask_id,period_ms,core,priority,latency_us
```

`latency_us` is `t_actual - t_scheduled`, sampled at the entry of `execute()`.
Deadlines are implicit, so a job missed its deadline when `latency_us` exceeds
`period_ms * 1000`.

## `heuristics-lowsat/` — 96 subtasks, 22,800 jobs

Plan: `plans/deployment_plan_lowsat.json` (32 tasks x 3 subtasks, periods 1/2/3/4/6/12 ms).

## `heuristics-highsat/` — 162 subtasks, 69,000 jobs

Plan: `plans/deployment_plan_sat.json` (54 tasks x 3 subtasks, same period grid).

## How each file was produced

There is no command-line flag for the allocation strategy: `show_alloc` and
`example_eval` both read it from the `allocation` block of the plan. Each of the
six datasets therefore comes from editing `"strategy"` in the plan, running the
harness, and renaming its fixed output name:

```bash
# for STRATEGY in first_fit best_fit worst_fit, and PLAN in
# plans/deployment_plan_lowsat.json (lowsat) / plans/deployment_plan_sat.json (highsat):
#   1. set "strategy": "<STRATEGY>" in the "allocation" block of <PLAN>
#   2. run the harness
sudo ./example_eval <PLAN> 50
#   3. example_eval always writes ./latency_samples.csv, so rename it
mv latency_samples.csv results/heuristics-<lowsat|highsat>/<STRATEGY>.csv
```

The `latency_by_core_*.png` figures are derived from the CSVs in the same
directory and can be regenerated at any time, on any machine:

```bash
python3 scripts/plot_latency.py results/heuristics-highsat/worst_fit.csv
mv latency_boxplot_by_core.png results/heuristics-highsat/latency_by_core_worst_fit.png
```

The same run also writes one `latency_boxplot_by_subtask_core<N>.png` per core.
Those are not stored here: with 96 to 162 subtasks they are unwieldy, and the
command above regenerates them on demand.

## Summary, recomputed from the files in this directory

Latency in ms. Jitter is the per-subtask peak-to-peak spread averaged over
subtasks. These are the numbers reported in `docs/tabelas-heuristicas.tex`.

| Dataset | Heuristic | Mean | Max | Jitter | Misses | Rate |
|---|---|---:|---:|---:|---:|---:|
| lowsat | First Fit | 16.503 | 582.118 | 151.701 | 3,276 | 14.37% |
| lowsat | Best Fit | 16.256 | 575.977 | 149.342 | 3,646 | 15.99% |
| lowsat | Worst Fit | 1.025 | 10.695 | 5.091 | 511 | 2.24% |
| highsat | First Fit | 81.994 | 610.568 | 162.219 | 63,524 | 92.12% |
| highsat | Best Fit | 100.294 | 582.477 | 196.663 | 66,753 | 96.74% |
| highsat | Worst Fit | 101.017 | 175.483 | 173.719 | 68,676 | 99.53% |

`heuristics-highsat/first_fit.csv` holds 68,959 jobs instead of 69,000: subtask
153 (T = 12 ms, core 0, priority 40) ran 9 of its 50 jobs and the remaining 41
were never dispatched. A job that never runs is not counted as a miss, so the
First Fit miss rate on that row is understated. The footnote of the table in
`docs/tabelas-heuristicas.tex` says the same.
