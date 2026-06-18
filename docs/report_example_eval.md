# Report: Evaluation Harness and Improvements — `example_eval.cpp`

## 1. Overview

`examples/example_eval.cpp` is the scheduling-quality evaluation harness for the MCFlow project. It loads a real deployment plan from JSON, fires subtasks at their configured periods, and collects per-subtask timing metrics — without touching the dispatcher itself.

The core question it answers for every subtask in the task graph:

- **How late did the scheduler actually start it relative to its scheduled release?** (latency)
- **How much did that delay vary across jobs?** (jitter)
- **How often did the delay exceed the configured deadline?** (deadline misses)

---

## 2. Evaluation Methodology

### 2.1 Execution Pipeline

The evaluation runs in 9 main steps:

| # | Step | Description |
|---|------|-------------|
| 1 | Parse plan | Reads `<deployment_plan.json>` via `JsonParser` |
| 2 | Predecessor map | Builds `preds[downstream] → [upstreams]` from connection entries |
| 3 | Shared state | Allocates an `atomic<double>[]` array (one slot per subtask id) to simulate data flow |
| 4 | Tick parameters | Computes `min_p` (smallest period), `lcm_p` (LCM of all periods), and `ticks = 4 × lcm_p / min_p` |
| 5 | Object allocation | Creates all `Subtask` instances on the heap; pre-reserves metric vector capacity |
| 6 | Instrumentation | Injects `execute()` lambdas that capture timestamps and accumulate latency samples |
| 7 | DAG construction | Builds the dependency graph for `TeamManager` |
| 8 | Initialization | Calls `tm.initialize()` — creates dispatchers grouped by (core, priority) |
| 9 | Run + collect | Runs the tick loop, drains for 50 ms, calls `tm.stop()` |

### 2.2 Metrics

For each subtask `s`, on every `execute()` call:

```
t_scheduled = s->next_release_ns - s->period_ns
t_actual    = Dispatcher::monotonic_ns()   // captured at execute() entry
latency     = t_actual - t_scheduled       // always ≥ 0 in a causal system
```

The dispatcher advances `s->next_release_ns += s->period_ns` **before** calling `execute()`, so subtracting `period_ns` recovers the scheduled release instant for that specific job.

Aggregates reported at the end:

| Metric | Computation |
|--------|-------------|
| `Lat_min` | `min(latency_ns)` over all jobs |
| `Lat_mean` | arithmetic mean of `latency_ns` |
| `Lat_max` | `max(latency_ns)` over all jobs |
| `Jitter` | `Lat_max − Lat_min` (peak-to-peak) |
| `Misses` | count of jobs where `latency > deadline_ns` |

All values are reported in **microseconds** in the output table.

---

## 3. Lambda Instrumentation

Each component type has a lightweight `execute()` lambda capturing only what it needs:

### Source
```cpp
s->execute = [v, id, s, &m, dl] {
    uint64_t t_actual = Dispatcher::monotonic_ns();
    v[id].store(v[id].load(relaxed) + 1.0, relaxed);  // simulate data production
    // compute latency, check deadline
};
```

### Intermediate
```cpp
s->execute = [v, id, pred, s, &m, dl] {
    uint64_t t_actual = Dispatcher::monotonic_ns();
    v[id].store(v[pred].load(relaxed) * 2.0, relaxed); // read predecessor, transform
    // compute latency, check deadline
};
```

### Sink
```cpp
s->execute = [v, pred, s, &m, dl] {
    uint64_t t_actual = Dispatcher::monotonic_ns();
    (void)v[pred].load(relaxed);                        // consume data (no write)
    // compute latency, check deadline
};
```

All data-flow accesses use `memory_order_relaxed` to minimize synchronization overhead during measurement.

---

## 4. Tick Loop and Time Control

The tick loop is the harness's event generator. On each iteration it determines which sources to activate using `tick % (period / min_period) == 0`, implementing rate-monotonic activation. The evolution of its sleep mechanism is the main axis of improvement across commits (see Section 5).

Current implementation:

```cpp
uint64_t next_tick_ns = Dispatcher::monotonic_ns() + min_p;
for (int tick = 1; tick <= ticks; ++tick) {
    struct timespec ts;
    ts.tv_sec  = next_tick_ns / 1'000'000'000ULL;
    ts.tv_nsec = next_tick_ns % 1'000'000'000ULL;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

    for (auto& [id, p] : sources)
        if (static_cast<uint64_t>(tick) % (p / min_p) == 0)
            tm.notify(id);

    next_tick_ns += min_p;
}
```

`TIMER_ABSTIME` tells the kernel to sleep until the specified absolute instant rather than for a relative duration from the call site — the most precise form of periodic timing available in POSIX.

---

## 5. Improvements Applied

### 5.1 Initial version — commit `51450aa` ("Evaluation of the scheduler")

Established the full harness with:

- Per-subtask metrics: latency, execution time (`exec_ns`), variance, and standard deviation of jitter
- End-to-end pipeline latency measurement
- Tick loop with **relative sleep**: `std::this_thread::sleep_for(nanoseconds(min_p))`

**Problem with relative sleep:** each tick slept `min_p` nanoseconds from the moment the sleep call was made — which was already slightly after the ideal instant. The error accumulated tick-by-tick, causing the harness to drift out of sync with the scheduler over longer runs.

---

### 5.2 Simplification — commit `9c16cab` ("Architecture.md, deletion of demultiplexer.hpp")

**Motivation:** execution-time metrics (`exec_ns`) and end-to-end latency measured aspects that depended on the mock workload (arithmetic operations), not on scheduler quality. They were removed to focus the report on what actually matters.

**Changes:**
- Removed `exec_ns`, variance, stddev, and the end-to-end latency report section
- Jitter simplified to peak-to-peak — but the header comment still said "variance/σ²" (inconsistency fixed later)
- Sleep remained relative — drift problem not yet resolved

---

### 5.3 Metric and sleep fixes — commit `c6d68e5` ("Fix sleep method and table headers")

Two independent and important corrections:

#### Correct deadline miss tracking

**Before:** `deadline_misses` was checked against `period_ns` — an incorrect proxy, since the real-time deadline is generally shorter than the period.

**After:** added `deadline_ns` field to `SubtaskMetrics`, populated from the JSON. Deadline misses are now verified against the subtask's actual configured deadline.

#### Drift-free relative sleep

**Before:**
```cpp
std::this_thread::sleep_for(std::chrono::nanoseconds(min_p));
```

**After:**
```cpp
auto t_next = std::chrono::steady_clock::now();
// inside the loop:
t_next += std::chrono::nanoseconds(min_p);
std::this_thread::sleep_until(t_next);
```

**Impact:** `t_next` is advanced by the fixed period every iteration, regardless of when the sleep actually wakes up. Per-tick overshoots no longer accumulate. Drift is eliminated.

#### Table headers

Columns renamed and aligned to reflect the actual metrics: `Lat_min`, `Lat_mean`, `Lat_max`, `Jitter`.

---

### 5.4 Absolute POSIX sleep — commit `c0f0399`

*Applied via external collaborator diff.*

**Motivation:** `std::this_thread::sleep_until` uses `steady_clock` from libstdc++, which may not be the same clock as `Dispatcher::monotonic_ns()` (which reads `CLOCK_MONOTONIC` directly via `clock_gettime`). Using the same clock in both the harness and the dispatcher ensures that measured latencies are directly comparable to the dispatcher's internal time base.

**Changes:**

1. `#include <time.h>` added — required for `clock_nanosleep`, `timespec`, `CLOCK_MONOTONIC`, `TIMER_ABSTIME`.

2. Tick loop rewritten with `clock_nanosleep`:

   ```cpp
   // Before (c6d68e5):
   auto t_next = std::chrono::steady_clock::now();
   t_next += std::chrono::nanoseconds(min_p);
   std::this_thread::sleep_until(t_next);

   // After (c0f0399):
   uint64_t next_tick_ns = Dispatcher::monotonic_ns() + min_p;
   struct timespec ts;
   ts.tv_sec  = next_tick_ns / 1'000'000'000ULL;
   ts.tv_nsec = next_tick_ns % 1'000'000'000ULL;
   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
   ```

3. Header comment corrected to match the actual implementation:
   - `"Reported as variance (σ²) and standard deviation (σ)."` → `"Reported as max_latency - min_latency (μs)."`

**Impact:** harness and dispatcher now share `CLOCK_MONOTONIC` as their time reference. Measured latencies are accurate and reproducible across runs.

---

## 6. Summary of Improvements

| Commit | Problem Solved | Before | After |
|--------|---------------|--------|-------|
| `9c16cab` | Irrelevant metrics obscured scheduling quality | `exec_ns`, stddev, E2E latency | Latency per subtask only |
| `c6d68e5` | Deadline miss checked against wrong value | `latency > period_ns` | `latency > deadline_ns` (from JSON) |
| `c6d68e5` | Tick drift accumulated over long runs | `sleep_for(min_p)` | `sleep_until(t_next)` with fixed `t_next` |
| `c0f0399` | Harness clock ≠ dispatcher clock | `steady_clock + sleep_until` | `CLOCK_MONOTONIC + clock_nanosleep(ABSTIME)` |
| `c0f0399` | Stale jitter description in header comment | "variance/σ²" | "peak-to-peak (max − min)" |

---

## 7. Terminal Output

```
=== Scheduling Evaluation: plan.json ===
Tasks: 3  Subtasks: 6  Dispatchers: 2
Tick: 10 ms  LCM: 60 ms  Ticks: 24  (240 ms)
Collecting metrics (no output during run)...

=== Latency & Jitter per Subtask ===
ID  Period(ms)  Core  Prio  Jobs  Lat_min(us)     Lat_mean(us)    Lat_max(us)     Jitter(us)      Misses
-----------------------------------------------------------------------------------------------------------------
1   10          0     90    24    12.345          15.210          28.900          16.555          0
2   20          0     80    12    11.200          14.800          25.100          13.900          0
...

Total jobs: 72  Deadline misses: 0  Miss rate: 0.00%
```

Column reference:

| Column | Unit | Meaning |
|--------|------|---------|
| `ID` | — | Subtask identifier from JSON |
| `Period(ms)` | ms | Activation period |
| `Core` | — | CPU affinity |
| `Prio` | — | SCHED_FIFO priority |
| `Jobs` | — | Number of executions collected |
| `Lat_min` | μs | Minimum observed latency |
| `Lat_mean` | μs | Mean latency |
| `Lat_max` | μs | Maximum observed latency |
| `Jitter` | μs | `Lat_max − Lat_min` |
| `Misses` | — | Jobs where `latency > deadline_ns` |

> **Tip:** run with `sudo` to enable `SCHED_FIFO` and obtain cleaner measurements free from OS scheduler interference.

```bash
sudo ./example_eval deployment_plan.json
```
