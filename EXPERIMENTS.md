# Running the experiments

Every command below is meant to be run from the root of the repository. Each
one states what it prints and which files it writes, because most of the
binaries write to the current directory under a fixed name and overwrite
previous runs without asking.

For the data and figures these commands produced for the article, see
[results/README.md](results/README.md).

## Contents

- [0. Before you start](#0-before-you-start)
- [1. Build](#1-build)
- [2. Run the tests](#2-run-the-tests)
- [3. Inspect an allocation without running anything](#3-inspect-an-allocation-without-running-anything)
- [4. Run a pipeline](#4-run-a-pipeline)
- [5. Measure latency, jitter and deadline misses](#5-measure-latency-jitter-and-deadline-misses)
- [6. Compare allocation heuristics (the tables in the article)](#6-compare-allocation-heuristics-the-tables-in-the-article)
- [7. Compare heuristics over many runs (does not currently build)](#7-compare-heuristics-over-many-runs-does-not-currently-build)
- [8. Generate random deployment plans](#8-generate-random-deployment-plans)
- [9. Find where the scheduler saturates](#9-find-where-the-scheduler-saturates)
- [10. Plot the results](#10-plot-the-results)
- [Deployment plans in this repository](#deployment-plans-in-this-repository)
- [Pitfalls](#pitfalls)

## 0. Before you start

**Hardware and kernel.** The numbers in the article come from a Raspberry Pi 5
(Cortex-A76) running Linux with `PREEMPT_RT`. Everything builds and runs on an
ordinary desktop kernel, and that is fine for exploring the tool, but latency
measured without `PREEMPT_RT` is not comparable to anything reported here.

**Run as root.** The dispatcher threads request `SCHED_FIFO`. Without root you
get this on every dispatcher, and the run continues under the default
scheduler:

```
[Dispatcher core=0] warning: RT priority not applied (run with sudo)
```

Measurements taken with that warning present are not comparable to measurements
taken as root. This is the single most common way to get meaningless numbers.

**Pin the CPU frequency** before measuring, so the governor does not change
clock speed mid-run:

```bash
sudo cpupower frequency-set -g performance
```

**Do not change the optimization level of the benchmarks.** The `wcet_ns`
values written in every deployment plan were measured with exactly the flags in
[Makefile:23](Makefile:23) (`-std=gnu89 -O0 -fno-builtin -fno-stack-protector`)
on a Raspberry Pi 5. Rebuilding the benchmark objects at another `-O` level
silently invalidates all of them: the plans keep their old numbers, the
allocator keeps trusting them, and nothing warns you.

**Python dependencies** for the scripts in `scripts/`:

```bash
pip install pandas matplotlib
```

`random_plan.py` and `saturation_sweep.py` use only the standard library, so
they run on a bare board with no `pip` available.

## 1. Build

```bash
make examples
```

Builds every example, including `example_eval`. The middleware sources live
in `src/`, which the `Makefile` puts on the include path, so the examples and
tests include its headers by bare name. The first build also compiles
the six Malardalen benchmarks into `wcet_bench/obj/` and runs `objcopy` on each
one, which is why `binutils` is a requirement.

Individual targets, for when you only need one:

```bash
make show_alloc example_eval example_from_plan
```

```bash
make clean
```

## 2. Run the tests

```bash
make test
```

Six suites, 35 assertions, covering phases 0 to 5: parser, DAG, ring buffer,
component model, dispatcher, team manager. They pass without root; the team
manager suite prints the `RT priority not applied` warning, which is expected
there.

## 3. Inspect an allocation without running anything

`show_alloc` parses a plan, lets the allocator assign cores, and prints the
result. Nothing is executed, so it needs no root and takes no time.

```bash
make show_alloc && ./show_alloc plans/deployment_plan_sat.json
```

It prints the `allocation` block in effect, one line per subtask with its
period, WCET, utilization and assigned core, and a per-core load bar:

```
plan      : plans/deployment_plan_sat.json
strategy  : first_fit
sort_by   : none
weight    : utilization
num_cores : 3
capacity  : 0
...
load per core (utilization):
  core 0  [######..................................]  0.16945
```

This is the fastest way to check what a heuristic actually did before spending
a measurement run on it.

## 4. Run a pipeline

```bash
sudo ./example_from_plan plans/deployment_plan.json 4
```

The second argument is the number of hyperperiods to simulate; it defaults to
4 when omitted. Source subtasks fire at their declared period for
`hyperperiods x LCM(source periods)`. The components have demo semantics: a
source increments a counter, an intermediate doubles its predecessor's value, a
sink prints it.

## 5. Measure latency, jitter and deadline misses

This is the measurement harness used for the article.

```bash
sudo ./example_eval plans/deployment_plan_lowsat.json 50
```

Latency is `t_actual - t_scheduled`, sampled at the entry of `execute()`;
jitter is the peak-to-peak spread of latency across the jobs of one subtask.
Deadlines are implicit, so a job missed when its latency exceeded its period.

It prints a header with the run geometry, then one row per subtask:

```
=== Scheduling Evaluation: plans/deployment_plan.json ===
Tasks: 6  Subtasks: 18  Dispatchers: 6
Tick: 1 ms  LCM: 48 ms  Ticks: 48  (48 ms)
...
ID  Period(ms)  Core  Prio  Jobs  Lat_min(us)  Lat_mean(us)  Lat_max(us)  Jitter(us)  Misses
1   1           0     80    48    1.129        13.835        73.476       72.347      0
```

and writes every individual sample to **`latency_samples.csv`** in the current
directory, in the format `subtask_id,period_ms,core,priority,latency_us`. That
file is overwritten by the next run, so rename it before running again.

## 6. Compare allocation heuristics (the tables in the article)

This reproduces the two tables in `docs/tabelas-heuristicas.tex` and the
contents of `results/`.

There is no command-line flag for the allocation strategy: both `show_alloc`
and `example_eval` read it from the `allocation` block of the plan. Comparing
heuristics therefore means editing the plan between runs.

For each of the two load levels, `plans/deployment_plan_lowsat.json` (96 subtasks)
and `plans/deployment_plan_sat.json` (162 subtasks), and for each strategy:

```bash
sed -i 's/"strategy": "[a-z_]*"/"strategy": "worst_fit"/' plans/deployment_plan_lowsat.json
```

```bash
./show_alloc plans/deployment_plan_lowsat.json | tail -6
```

```bash
sudo ./example_eval plans/deployment_plan_lowsat.json 50
```

```bash
mv latency_samples.csv results/heuristics-lowsat/worst_fit.csv
```

Repeat with `first_fit` and `best_fit`, then with `plans/deployment_plan_sat.json`
into `results/heuristics-highsat/`. Restore the plan's original strategy when
you are done, so the file in the repository does not silently change meaning.

A low-saturation run of 50 hyperperiods takes about a minute of wall time; the
high-saturation one takes several, and produces 1.4 MB of samples.

## 7. Compare heuristics over many runs (does not currently build)

`examples/example_heuristic_eval.cpp` runs all fifteen heuristic combinations
(First/Best/Worst Fit, each plain and with the four period and utilization sort
criteria) back to back on a task set it generates itself, and
`scripts/run_heuristic_eval_stats.py` repeats that binary N times and aggregates
the results.

Neither works in this snapshot. The example includes
`wcet_components/wcet_components_rt.hpp`, a directory that is not part of the
repository, so `make example_heuristic_eval` fails with:

```
fatal error: wcet_components/wcet_components_rt.hpp: No such file or directory
```

and the script has no binary to drive. Both files are kept for reference; the
working way to compare heuristics is section 6, which is what the article's
tables are built from.

## 8. Generate random deployment plans

```bash
python3 scripts/random_plan.py --tasks 32 --seed 1 --platform pi5 -o plans/deployment_plan_random.json
```

Every task is a linear chain `source -> intermediate* -> sink` whose length is
drawn from `--min-subtasks`/`--max-subtasks` (default 2 to 5). Periods come
from `--periods` (default `1,2,3,4,6,12` ms), the benchmark of each subtask
from `--benchmarks`, and its `wcet_ns` from the measured table of the platform
selected by `--platform`. Priorities are rate-monotonic between `--prio-high`
and `--prio-low`. Always pass `--seed` if you want the plan back.

Load is not an input. You ask for N tasks and the script reports the
utilization that came out:

```
  subtasks         23
  util per core    0.056  (3 cores)
  dispatches/ms    8.9
```

The generated plan leaves `core` unset on every subtask, so it is the
`allocation` block, configurable through `--strategy`, `--sort-by`, `--weight`
and `--capacity`, that decides the placement at parse time.

Two platform tables are available. `--platform pi5` comes from the measurement
campaign in `wcet_bench/cycle_counter/RELATORIO.md`. `--platform pi4` was
back-filled from an older plan file and has no report behind it, so treat those
numbers as weaker evidence.

## 9. Find where the scheduler saturates

```bash
sudo python3 scripts/saturation_sweep.py --start 1 --stop 6 --step 0.5
```

Each step scales the shape of `plans/deployment_plan_lowsat.json` by a factor, writes
the plan, runs `example_eval` on it, and records the outcome in `--outdir`
(default `sweep_out/`): `sweep.csv` with one row per step, plus the generated
`plan_<level>.json` and the raw `latency_<level>.csv` of every step.

Two load axes are reported per step, and they do not saturate together:

- `util_per_core` — `sum(wcet/period) / cores`, what `allocator.hpp` reasons
  about;
- `dispatches_ms` — jobs released per millisecond. Each one pays a roughly
  constant queue, notify and clock cost that the utilization model does not
  see, which is what binds first in practice.

`--stop-at-miss PCT` aborts the sweep once the miss rate crosses a threshold,
which saves a lot of time when you only want to locate the knee.

## 10. Plot the results

```bash
python3 scripts/plot_latency.py results/heuristics-highsat/worst_fit.csv
```

Writes, **into the current directory** and always under the same names:

- `latency_boxplot_by_core.png` — one box per core;
- `latency_boxplot_by_subtask_core<N>.png` — one figure per core, one box per
  subtask, coloured by period.

Move or rename them right after generating, or the next run overwrites them.
The figures in `results/` were produced exactly this way.

When a long tail crushes the boxes, `--max-latency` drops it explicitly:

```bash
python3 scripts/plot_latency.py results/heuristics-lowsat/first_fit.csv --max-latency 600
```

The discard is never silent: the script prints how many samples were dropped
per core, stamps the count on both figures, and warns when more than 1% of the
data was discarded, since at that point the tail is part of the distribution
rather than an outlier set.

That warning fires on every dataset in `results/`. The command above discards
76% of the samples, and even the best-behaved run, low saturation with Worst
Fit, still has 4.4% of its jobs above 8 ms. Use the flag to look at the body of
the distribution, never to clean the data, and quote the discarded fraction
whenever you show a truncated figure. The figures stored in `results/` were
generated without it, on the full samples.

## Deployment plans in this repository

All plans live in `plans/`.

| Plan | Tasks | Subtasks | Purpose |
|---|---:|---:|---|
| `deployment_plan.json` | 6 | 18 | the small hand-written example, harmonic periods 1 to 48 ms |
| `deployment_plan_wcet.json` | 6 | 18 | same shape, with `wcet_ns` filled in |
| `deployment_plan_lowsat.json` | 32 | 96 | low saturation, first table of the article |
| `deployment_plan_lowsat_pi04.json` | 32 | 96 | same shape with the Pi 4 WCET table |
| `deployment_plan_sat.json` | 54 | 162 | high saturation, second table of the article |
| `deployment_plan_oversat.json` | 63 | 189 | past saturation |
| `deployment_plan_random.json` | 32 | 109 | example output of `scripts/random_plan.py` |

## Pitfalls

- **Fixed output names.** `example_eval` always writes `latency_samples.csv`,
  and `plot_latency.py` always writes `latency_boxplot_by_core.png`, in the
  current directory. Rename before the next run or lose the previous one.
- **The strategy lives in the plan file.** Editing it changes the meaning of
  every later run against that plan, including runs you did not intend to
  change. Restore it afterwards.
- **`capacity: 0` means no limit.** With `first_fit` and no capacity, the
  allocator legitimately fills core 0 first and may leave the other cores
  empty. That is the heuristic working, not a bug.
- **Utilization needs WCET.** A plan whose subtasks carry no `wcet_ns` has zero
  utilization everywhere, so any `weight: utilization` strategy sees all cores
  as equally empty and piles everything onto core 0.
  `docs/report_eval_rpi5.md` section 2.3 documents this in detail.
- **WCETs are platform-specific.** The values in the plans were measured on a
  Raspberry Pi 5. Running them elsewhere without re-measuring makes the
  allocator's arithmetic fiction.
