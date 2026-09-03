#!/usr/bin/env python3
"""
saturation_sweep.py — finds where the scheduler saturates by growing the task
set step by step and running evaluation on each generated plan.

Usage (run on the board, as root so evaluation gets SCHED_FIFO):
    sudo python3 scripts/saturation_sweep.py
    sudo python3 scripts/saturation_sweep.py --start 0.5 --stop 6 --step 0.5

Each step scales the plans/deployment_plan_lowsat.json shape by a factor, writes the
plan, runs the binary, and records the result. Output goes to --outdir:
    sweep.csv                     one row per step
    plan_<level>.json             the generated plan
    latency_<level>.csv           the raw samples of that run

Two load axes are reported per step, and they do not saturate together:

  util_per_core   sum(wcet/period) / cores. What allocator.hpp reasons about.
  dispatches_ms   jobs released per millisecond. Each one pays a roughly
                  constant queue+notify+clock cost that the utilization model
                  does not see, so this is what actually binds first.

Stdlib only, so it runs on a bare Raspberry Pi.
"""

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys

MS = 1_000_000

# RELATORIO.md sec. 4, measured on the Pi 5
HEAVY = {"source": ("matmult", 48050), "intermediate": ("bsort100", 30310),
         "sink": ("crc", 821)}
LIGHT = {"source": ("ud", 789), "intermediate": ("fft1", 746),
         "sink": ("statemate", 207)}

PRIO = {1: 90, 2: 80, 3: 70, 4: 60, 6: 50, 12: 40}
OUT = {"source": "double", "intermediate": "double", "sink": "void"}

# (period_ms, flavor, count at level 1.0) — the plans/deployment_plan_lowsat.json mix.
TEMPLATE = [
    (1,  "heavy", 6),
    (2,  "heavy", 6),
    (3,  "heavy", 4),
    (4,  "heavy", 4),
    (6,  "heavy", 4),
    (12, "heavy", 4),
    (12, "light", 4),
]


def build_plan(level, num_cores, strategy, sort_by):
    """Returns (plan_dict, stats_dict) for the template scaled by `level`."""
    tasks, conns = [], []
    sid = tid = 1

    for period_ms, flavor, base in TEMPLATE:
        spec = HEAVY if flavor == "heavy" else LIGHT
        count = max(1, round(base * level))
        for _ in range(count):
            first = sid
            subtasks = []
            for kind in ("source", "intermediate", "sink"):
                name, wcet = spec[kind]
                subtasks.append({
                    "id": sid,
                    "component_type": kind,
                    "priority": PRIO[period_ms],
                    "period_ns": period_ms * MS,
                    "deadline_ns": period_ms * MS,
                    "output_type": OUT[kind],
                    "config": {},
                    "wcet_ns": wcet,
                    "benchmark": name,
                })
                sid += 1
            conns.append({"upstream": first,     "downstream": first + 1})
            conns.append({"upstream": first + 1, "downstream": first + 2})
            tasks.append({"id": tid, "subtasks": subtasks})
            tid += 1

    plan = {
        "hosts": [{"name": "localhost", "address": "127.0.0.1"}],
        "allocation": {
            "strategy": strategy,
            "sort_by": sort_by,
            "weight": "utilization",
            "num_cores": num_cores,
            "capacity": 0,
        },
        "tasks": tasks,
        "connections": conns,
    }

    all_st = [s for t in tasks for s in t["subtasks"]]
    util = sum(s["wcet_ns"] / s["period_ns"] for s in all_st)
    disp = sum(1.0 / (s["period_ns"] / MS) for s in all_st)
    per_core = disp / num_cores
    slack_us = 1000.0 * (1.0 - util / num_cores)

    stats = {
        "tasks": len(tasks),
        "subtasks": len(all_st),
        "util_total": util,
        "util_per_core": util / num_cores,
        "dispatches_ms": disp,
        # Overhead each job may cost before the cores stop keeping up. Negative
        # means the declared work alone already exceeds capacity.
        "overhead_budget_us": slack_us / per_core if per_core else float("inf"),
    }
    return plan, stats


SUMMARY_RE = re.compile(
    r"Total jobs:\s*(\d+)\s+Deadline misses:\s*(\d+)\s+Miss rate:\s*([\d.]+)%")


def sample_stats(path):
    """mean / max latency and peak-to-peak jitter from a latency CSV."""
    lo, hi, total, n = float("inf"), 0.0, 0.0, 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            v = float(row["latency_us"])
            lo = min(lo, v)
            hi = max(hi, v)
            total += v
            n += 1
    if not n:
        return {}
    return {"lat_mean_us": total / n, "lat_max_us": hi, "jitter_us": hi - lo}


def run_step(binary, plan_path, hyperperiods, timeout):
    """Runs evaluation. Returns (summary_dict, error_string_or_None)."""
    try:
        p = subprocess.run([binary, plan_path, str(hyperperiods)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {}, f"timeout after {timeout}s"

    if p.returncode != 0:
        # An infeasible allocation throws out of JsonParser::parse.
        msg = (p.stderr or p.stdout).strip().splitlines()
        return {}, (msg[-1] if msg else f"exit {p.returncode}")

    m = SUMMARY_RE.search(p.stdout)
    if not m:
        return {}, "could not parse the summary line"

    return {"jobs": int(m.group(1)),
            "misses": int(m.group(2)),
            "miss_pct": float(m.group(3))}, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=float, default=1.0,
                    help="first scale factor (1.0 = the lowsat task set)")
    ap.add_argument("--stop", type=float, default=6.0, help="last scale factor")
    ap.add_argument("--step", type=float, default=0.5, help="increment")
    ap.add_argument("--cores", type=int, default=3)
    ap.add_argument("--strategy", default="worst_fit")
    ap.add_argument("--sort-by", default="none")
    ap.add_argument("--hyperperiods", type=int, default=20)
    ap.add_argument("--binary", default="./evaluation")
    ap.add_argument("--outdir", default="sweep_out")
    ap.add_argument("--timeout", type=int, default=300,
                    help="per-run timeout in seconds")
    ap.add_argument("--stop-at-miss", type=float, default=None, metavar="PCT",
                    help="stop once the miss rate exceeds this (default: run all)")
    args = ap.parse_args()

    if not os.access(args.binary, os.X_OK):
        sys.exit(f"{args.binary} not found or not executable; run `make evaluation` first")

    if os.geteuid() != 0:
        print("WARNING: not running as root, so evaluation cannot apply "
              "SCHED_FIFO and every number below will be dominated by CFS "
              "scheduling noise.\n", file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    sweep_path = os.path.join(args.outdir, "sweep.csv")

    cols = ["level", "tasks", "subtasks", "util_total", "util_per_core",
            "dispatches_ms", "overhead_budget_us", "jobs", "misses", "miss_pct",
            "lat_mean_us", "lat_max_us", "jitter_us", "error"]

    hdr = (f"{'level':>6} {'tasks':>6} {'subt':>6} {'U/core':>7} {'disp/ms':>8} "
           f"{'budget':>8} {'jobs':>7} {'misses':>7} {'miss%':>7} "
           f"{'mean_us':>10} {'max_us':>11}")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    level = args.start
    while level <= args.stop + 1e-9:
        tag = f"{level:g}".replace(".", "_")
        plan_path = os.path.join(args.outdir, f"plan_{tag}.json")

        plan, st = build_plan(level, args.cores, args.strategy, args.sort_by)
        with open(plan_path, "w") as f:
            json.dump(plan, f, indent=2)
            f.write("\n")

        summary, err = run_step(args.binary, plan_path, args.hyperperiods,
                                args.timeout)

        row = {"level": level, "error": err or "", **st, **summary}

        # evaluation writes latency_samples.csv into the cwd.
        if not err and os.path.exists("latency_samples.csv"):
            dest = os.path.join(args.outdir, f"latency_{tag}.csv")
            shutil.move("latency_samples.csv", dest)
            row.update(sample_stats(dest))

        rows.append(row)

        if err:
            # Full text stays in sweep.csv; an infeasible allocation lists every
            # unplaced subtask and would swamp the table.
            short = err if len(err) <= 72 else err[:69] + "..."
            print(f"{level:>6g} {st['tasks']:>6} {st['subtasks']:>6} "
                  f"{st['util_per_core']:>7.3f} {st['dispatches_ms']:>8.1f} "
                  f"{st['overhead_budget_us']:>8.1f}   -> {short}")
        else:
            print(f"{level:>6g} {st['tasks']:>6} {st['subtasks']:>6} "
                  f"{st['util_per_core']:>7.3f} {st['dispatches_ms']:>8.1f} "
                  f"{st['overhead_budget_us']:>8.1f} {row['jobs']:>7} "
                  f"{row['misses']:>7} {row['miss_pct']:>7.2f} "
                  f"{row.get('lat_mean_us', 0):>10.1f} "
                  f"{row.get('lat_max_us', 0):>11.1f}")

        # Written every step so an interrupted sweep still leaves usable data.
        with open(sweep_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)

        if (args.stop_at_miss is not None and not err
                and row["miss_pct"] > args.stop_at_miss):
            print(f"\nStopping: miss rate {row['miss_pct']:.2f}% exceeded "
                  f"--stop-at-miss {args.stop_at_miss:g}%")
            break

        level += args.step

    print(f"\nWrote {sweep_path}")

    clean = [r for r in rows if not r["error"] and r.get("miss_pct") is not None]
    good = [r for r in clean if r["miss_pct"] == 0.0]
    if good:
        last = good[-1]
        print(f"Last level with zero misses: {last['level']:g} "
              f"({last['subtasks']} subtasks, U/core {last['util_per_core']:.3f}, "
              f"{last['dispatches_ms']:.1f} dispatches/ms)")
    elif clean:
        print("No level reached zero misses; lower --start.")


if __name__ == "__main__":
    main()
