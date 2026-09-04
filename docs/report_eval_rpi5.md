# Report: Scheduling Evaluation on Raspberry Pi 5 / PREEMPT_RT

Results of `sudo ./evaluation plans/deployment_plan.json` on the target hardware.
Written in English to match its sibling reports (`report_example_eval.md`,
`report_heuristic_eval.md`).

## 1. Setup

| | |
|---|---|
| Hardware | Raspberry Pi 5 (Cortex-A76) |
| OS | Linux with `PREEMPT_RT` |
| Scheduling | `SCHED_FIFO` (run as root — RT priority applied) |
| Plan | `plans/deployment_plan.json` — 6 tasks × 3 subtasks, harmonic periods 1/2/6/12/24/48 ms |
| Allocation | automatic: `worst_fit` + `priority_desc`, `weight: count`, 3 cores |
| Run length | 192 ticks of 1 ms = **192 ms** (4 × LCM) |
| Metric | `latency = t_actual − t_scheduled`, sampled at `execute()` entry |

The `Core` column in the results was **not** hand-written. Section 2 documents how it
was produced, and Section 6.2 explains why that specific outcome limits what the data
can prove.

## 2. How the cores were allocated

### 2.1 The plan no longer names cores

`plans/deployment_plan.json` declares no `core` on any subtask. Instead it carries a top-level
block:

```json
"allocation": {
  "strategy":  "worst_fit",
  "sort_by":   "priority_desc",
  "weight":    "count",
  "num_cores": 3,
  "capacity":  0
}
```

The assignment happens at **parse time**, before the runtime exists.
`JsonParser::parse` (`parser_json.cpp`) defaults each missing `core` to the sentinel
`CORE_UNASSIGNED` (`-1`), notices that at least one subtask is unassigned, and calls
`allocator::apply_auto_allocation(plan)` before returning. By the time `evaluation`
builds its DAG and hands entries to `TeamManager`, every `core` is already a concrete
number. Nothing downstream — dispatcher, team manager, the harness — knows an allocator
ran; they see a fully-specified plan exactly as if it had been typed by hand.

### 2.2 The three knobs, and what they compose into

| Field | Value | Meaning |
|---|---|---|
| `weight` | `count` | every subtask weighs `1.0`, so a core's "load" is its subtask count |
| `sort_by` | `priority_desc` | `stable_sort` on `priority`, highest first |
| `strategy` | `worst_fit` | place on the core with the **most** remaining room; ties go to the lowest core index |

The combination is the whole trick: Worst Fit over unit weights reduces to **"put it on
the least-loaded core"**. Walking the subtasks from highest to lowest priority and always
handing the next one to the emptiest core is a priority-ordered round-robin — which is
exactly the behaviour wanted, expressed through the bin-packing heuristics that already
existed in `allocator.hpp` rather than a new special-purpose code path.

### 2.3 Why `count` and not `utilization`

The classical metric here is utilization, `wcet_ns / period_ns`. It is not used, because
**the plan carries no `wcet_ns`**. With WCET absent, every subtask's utilization is `0.0`,
so every core looks equally empty forever, and Worst Fit's strict `>` tie-break hands every
one of them to core 0. This is not hypothetical — running the same plan with
`"weight": "utilization"` produces:

```
subtasks por core:
  core 0: 18
```

All 18 subtasks on a single core, the other two idle. The `count` weight exists precisely
to avoid that degeneration. The price is stated plainly in Section 5: counting treats a
1 ms subtask and a 48 ms subtask as identical load, so the allocation balances *work items*,
not *processor demand*, and no schedulability bound is being tested.

### 2.4 Capacity

`"capacity": 0` means *derive it*. Under `count` weight the allocator sets it to
`ceil(n_subtasks / num_cores)` = `ceil(18 / 3)` = **6** subtasks per core.

Capacity is a feasibility ceiling, not the balancing mechanism — Worst Fit spreads the load
on its own, and here it never touches the ceiling. Its real job is to fail loudly: if a
subtask fits nowhere, `apply_auto_allocation` throws and names the unplaced ids rather than
silently overloading a core or leaving a `-1` to reach `CPU_SET()`.

### 2.5 Step-by-step trace

`priority_desc` is a **stable** sort, so subtasks that share a priority keep their
declaration order from the JSON. Within each task that order is source → intermediate →
sink, so the sorted sequence is:

```
prio 80: 1, 2, 3   |  prio 65: 4, 5, 6   |  prio 50: 7, 8, 9
prio 35: 10,11,12  |  prio 20: 13,14,15  |  prio  6: 16,17,18
        (source, intermediate, sink)  ×  6
```

Worst Fit then walks that sequence, load starting at `[0,0,0]` and capacity 6:

| Step | Subtask | Type | Prio | Load before | Remaining | Chosen core |
|---|---|---|---|---|---|---|
| 1 | 1 | source | 80 | `[0,0,0]` | `[6,6,6]` | **0** (tie → lowest) |
| 2 | 2 | intermediate | 80 | `[1,0,0]` | `[5,6,6]` | **1** |
| 3 | 3 | sink | 80 | `[1,1,0]` | `[5,5,6]` | **2** |
| 4 | 4 | source | 65 | `[1,1,1]` | `[5,5,5]` | **0** (tie → lowest) |
| 5 | 5 | intermediate | 65 | `[2,1,1]` | `[4,5,5]` | **1** |
| 6 | 6 | sink | 65 | `[2,2,1]` | `[4,4,5]` | **2** |
| … | | | | | | *cycle repeats* |

Every third step the load returns to a three-way tie, the tie-break resets to core 0, and
the cycle restarts identically. Final load: `[6,6,6]` — 6 subtasks per core, all sources on
core 0, all intermediates on core 1, all sinks on core 2.

This reproduces, exactly, the partition that used to be written by hand in the JSON.

### 2.6 The stage↔core alignment is a coincidence, not a design goal

It is tempting to read "sources on core 0, intermediates on core 1, sinks on core 2" as
something the allocator intends. It does not. That alignment survives only because three
independent facts happen to line up:

1. each priority level holds exactly **3** subtasks,
2. there are exactly **3** cores, and
3. the stable sort preserves source/intermediate/sink declaration order inside each level.

Break any one and it dissolves. Re-running the identical plan with `"num_cores": 4`:

| Subtask | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| Type | source | interm. | sink | source | interm. | sink |
| Core | 0 | 1 | 2 | **3** | **0** | **1** |

The stages scramble across cores immediately. The allocator is balancing counts; the
stage-wise partition on 3 cores is an artifact of the arithmetic. This matters for
interpreting the results — see Section 6.2, where it is the direct cause of a confound.

### 2.7 Hand-pinning still works

A subtask that *does* declare `"core": N` is honoured as fixed pinning: it keeps that core,
and its weight is pre-loaded into that core before packing begins, so the automatic
placements account for it. This makes partial migration possible and leaves an escape hatch
for cases that must be forced (isolating a particular sink, say). The run in this report
used no pinning — all 18 subtasks were placed automatically.

## 3. Raw results

```
ID  Period(ms)  Core  Prio  Jobs  Lat_min(us)  Lat_mean(us)  Lat_max(us)  Jitter(us)  Misses
1   1           0     80    192   1.518        26.564        67.925       66.407      0
2   1           1     80    192   1.093        23.583        64.796       63.703      0
3   1           2     80    192   1.203        20.121        80.729       79.526      0
4   2           0     65    96    0.407        29.737        103.935      103.528     0
5   2           1     65    96    0.426        29.329        102.657      102.231     0
6   2           2     65    96    0.519        28.474        132.848      132.329     0
7   6           0     50    32    0.352        48.255        123.981      123.629     0
8   6           1     50    32    0.500        46.791        122.240      121.740     0
9   6           2     50    32    0.278        46.204        121.537      121.259     0
10  12          0     35    16    0.296        32.426        59.149       58.853      0
11  12          1     35    16    0.296        31.240        59.056       58.760      0
12  12          2     35    16    0.259        30.880        59.279       59.020      0
13  24          0     20    8     0.407        37.650        85.254       84.847      0
14  24          1     20    8     0.518        37.087        84.310       83.792      0
15  24          2     20    8     0.185        36.434        83.421       83.236      0
16  48          0     6     4     0.574        26.144        50.831       50.257      0
17  48          1     6     4     0.537        26.038        50.553       50.016      0
18  48          2     6     4     0.241        25.135        49.386       49.145      0

Total jobs: 1044  Deadline misses: 0  Miss rate: 0.00%
```

## 4. What the run got right

**Every expected activation fired.** 1044 jobs is exactly `3 × (192+96+32+16+8+4)`.
No release was dropped, and no subtask was starved — including priority 6 at the
48 ms boundary, where all six sources are released in the same tick. For a
partitioned fixed-priority runtime this is the first thing that has to hold, and it does.

**The dispatcher fast path is cheap.** Minimum latency lands between **0.185 µs and
1.518 µs**. That is the full `notify → eventfd → wake → execute()` path when
uncontended, and it means the middleware itself adds well under 2 µs of overhead.

**Latency is bounded well inside the deadlines.** The worst single sample across the
whole run was **132.8 µs** (subtask 6). Relative to its own deadline, the tightest
case was subtask 3 at **8.1%** of its 1 ms deadline — roughly **12× of headroom**.

## 5. The headline number is weaker than it looks

> `Deadline misses: 0 — Miss rate: 0.00%`

This should not be read as "the task set is schedulable". Two reasons:

**The metric is start delay, not response time.** `evaluation.cpp:191` tests
`latency > deadline_ns`, where `latency` is how late the subtask *started*. It never
measures when the subtask *finished*. A subtask that starts on time and then overruns
its deadline is counted as a success.

**The threshold is far out of reach.** The tightest deadline is 1 ms = 1000 µs, and the
worst observed start delay anywhere was 132.8 µs. Registering even one miss would take
a ~7.5× degradation. A metric that cannot fire is not evidence.

What the run *does* establish: releases were never catastrophically late, and the system
has a large margin on the release path. Schedulability remains unproven — there is still
no WCET in the plan, so, as Section 2.3 describes, the allocation balances subtask counts
and no utilization bound is tested anywhere.

## 6. Two effects in the data

### 6.1 Priority ordering holds — where the samples are sufficient

Pooling the three cores at each priority level:

| Prio | Period | Jobs | Mean (µs) | Max (µs) |
|---|---|---|---|---|
| 80 | 1 ms | 576 | **23.42** | 80.7 |
| 65 | 2 ms | 288 | **29.18** | 132.8 |
| 50 | 6 ms | 96 | **47.08** | 124.0 |
| 35 | 12 ms | 48 | 31.52 | 59.3 |
| 20 | 24 ms | 24 | 37.06 | 85.3 |
| 6 | 48 ms | 12 | 25.77 | 50.8 |

Mean latency rises from 23 → 29 → 47 µs as priority falls from 80 → 65 → 50. That is
exactly what partitioned fixed-priority predicts, and those three levels carry 576, 288
and 96 samples — enough for the gaps to be real.

The sequence then *drops* at priorities 35, 20 and 6, which looks like the ordering
breaks down. It almost certainly does not — **those rows carry 48, 24 and 12 samples**.
At n=12 the standard error on the mean is on the order of the differences being compared,
so the tail is not measuring priority, it is measuring noise.

**The `Lat_max` and `Jitter` columns are worse than uninformative across rows — they are
actively misleading.** Maximum is a monotonically increasing function of sample count:
subtask 3 drew 192 samples and subtask 18 drew 4. The comfortable 49.4 µs max at priority 6
does not mean it behaves better than priority 80's 80.7 µs; it means the dice were rolled
4 times instead of 192. Never rank subtasks by `Lat_max` in this table.

Two further notes on the columns. Since `Lat_min ≈ 0`, `Jitter = max − min ≈ max` — the
jitter column carries no information the max column does not already have. And the run is
**192 ms long**, which cannot characterize tail latency at all; `cyclictest`-style RT
characterization runs for hours precisely because the worst case hides in the tail. Treat
132.8 µs as a *lower bound* on the true worst case, not an estimate of it.

### 6.2 A small, perfectly systematic core effect — which is confounded

At every single priority level, mean latency orders `core 0 > core 1 > core 2`:

| Prio | core 0 | core 1 | core 2 | core0 − core2 |
|---|---|---|---|---|
| 80 | 26.564 | 23.583 | 20.121 | **+6.44** |
| 65 | 29.737 | 29.329 | 28.474 | +1.26 |
| 50 | 48.255 | 46.791 | 46.204 | +2.05 |
| 35 | 32.426 | 31.240 | 30.880 | +1.55 |
| 20 | 37.650 | 37.087 | 36.434 | +1.22 |
| 6 | 26.144 | 26.038 | 25.135 | +1.01 |

Six out of six in the same direction (mean gap 2.25 µs). Under a sign test that is
p ≈ 0.03 — small, but not chance.

**It cannot be attributed from this data**, and the reason traces straight back to
Section 2.6. The count-balancing allocation happened to put *all sources* on core 0,
*all intermediates* on core 1 and *all sinks* on core 2 — so "which core" and "which
pipeline stage" vary together, perfectly. At least two explanations fit equally well:

1. **Core 0 is busier.** Linux defaults IRQ affinity to CPU 0, so core 0 absorbs
   interrupt load the other cores do not see.
2. **Sources are notified first.** `TeamManager::notify()` serializes on `state_mutex_`
   (`team_manager.cpp:141`), and the tick loop notifies sources in plan order, so the
   head of the chain pays a cost the tail does not.

Two cheap experiments separate them, both using artifacts already in the repo:

- **`plans/deployment_plan_test.json`** partitions by pipeline instead of by stage (task 1 →
  core 0, tasks 2–3 → core 1, tasks 4–6 → core 2), mixing stages onto every core.
- **`"num_cores": 4`** in the existing plan scrambles the stages on its own (§2.6),
  with no new file at all.

If the gap follows core 0, hypothesis 1 holds; if it follows the sources, hypothesis 2 holds.

## 7. Recommended next steps

Roughly in order of what would most improve the evidence:

1. **Run far longer.** 192 ms is a smoke test. Raising the `4 × LCM` multiplier in
   `evaluation.cpp:137` to cover minutes would make `Lat_max` mean something and give
   the low-rate subtasks enough samples to compare.
2. **Stop comparing `Lat_max` across unequal `Jobs`.** Either equalize sample counts or
   report a percentile (p99/p99.9) plus the sample count. The raw data is already in
   `latency_samples.csv`, one row per job, so this is post-processing only.
3. **Break the core/stage confound** (§6.2) — the `num_cores: 4` variant costs one line.
4. **Measure response time, not just start delay**, if deadline misses are to mean
   anything (§5).
5. **Tune the Pi** before trusting the tail: `isolcpus`, `irqaffinity` off the RT cores,
   and the `performance` CPU governor. A 132 µs worst case is high for a PREEMPT_RT Pi 5.
6. **Measure WCET** to unlock utilization-based allocation and a real schedulability
   bound — the path is already open via `"weight": "utilization"` in the plan's
   `allocation` block, and §2.3 shows it is inert until WCET exists.

## 8. Bottom line

The runtime works on the target: no lost activations, sub-2 µs uncontended overhead,
and worst-case start delay ~12× inside the tightest deadline. Where sample counts allow
a conclusion, latency tracks priority the way partitioned fixed-priority says it should.
The cores behind those numbers were chosen automatically from the declared priorities,
and reproduced the previous hand-written partition exactly.

What this run does **not** show is schedulability, tail behaviour, or any effect of core
placement — the miss metric cannot fire, 192 ms is too short to find the worst case, and
core placement is confounded with pipeline stage. The numbers are a good sanity check,
not yet a characterization.
