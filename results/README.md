# Measurement data

Every file here was produced by a command listed below, on the hardware
described below. Nothing was edited by hand.

## Platform

| | |
|---|---|
| Board | Raspberry Pi 5 (Cortex-A76) and Raspberry Pi 4 Model B (Cortex-A72)|
| Kernel | Linux with `PREEMPT_RT` |
| Scheduling | `SCHED_FIFO`, obtained by running the binary as root |
| CPU governor | `performance` |
| Compiler flags | as fixed in the `Makefile`; the benchmark objects **must** stay at `-O0`, or every `wcet_ns` in the deployment plans becomes wrong |
| Run length | 50 hyperperiods (`evaluation` second argument) |
| Cores used | 3, as declared in the `allocation` block of each plan |

## CSV format

One row per job, written by `scripts/evaluation.cpp`:

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

There is no command-line flag for the allocation strategy: `allocate` and
`evaluation` both read it from the `allocation` block of the plan. Each of the
six datasets therefore comes from editing `"strategy"` in the plan, running the
harness, and renaming its fixed output name:

```bash
# for STRATEGY in first_fit best_fit worst_fit, and PLAN in
# plans/deployment_plan_lowsat.json (lowsat) / plans/deployment_plan_sat.json (highsat):
#   1. set "strategy": "<STRATEGY>" in the "allocation" block of <PLAN>
#   2. run the harness
sudo ./evaluation <PLAN> 50
#   3. evaluation always writes ./latency_samples.csv, so rename it
mv latency_samples.csv results/heuristics-<lowsat|highsat>/<STRATEGY>.csv
```

The `latency_by_core_*.png` figures are derived from the CSVs in the same
directory and can be regenerated at any time, on any machine:

```bash
python3 scripts/plot_latency.py results/heuristics-highsat/worst_fit.csv
mv latency_boxplot_by_core.png results/heuristics-highsat/latency_by_core_worst_fit.png
```