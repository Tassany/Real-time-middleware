#!/usr/bin/env python3
"""
plot_taskset_scheduled.py — "taskset scheduled" vs. "number of cores", the
scalability plot style used by Casini2018/Fonseca2016 (neither is implemented
in this repo; this reproduces the shape of that plot for MCFlow's own
allocator, not those two schedulers).

Why a single fixed plan does not work: allocator::apply_auto_allocation
(src/allocator.hpp) is all-or-nothing — if any subtask does not fit, the
whole plan is rejected (see the "unplaced subtask(s)" error from `allocate`).
A single deployment plan therefore gives a step function (0 accepted below
some core count, all of them above it), not the smooth curve of the
reference plot. That curve comes from testing a whole POPULATION of
independent random tasksets per core count and counting how many are
feasible — same idea here: --seeds independent random plans (same template:
--tasks/--benchmarks/--platform/--periods/--strategy/--sort-by), generated
fresh for every core count via random_plan.py, checked with the real
`allocate` binary (fast: pure bin-packing, no root, no execution).

Usage:
    python3 scripts/plot_taskset_scheduled.py
    python3 scripts/plot_taskset_scheduled.py --seeds 200 --cores 1,2,3,4,6,8,12,16,20,24
    python3 scripts/plot_taskset_scheduled.py --platform pi4 --benchmarks matmult,crc,ud

Requires the `allocate` binary already built at the repo root (`make allocate`).

Outputs:
    --csv     one row per (cores, seeds_ok, seeds_total)
    --output  the plot (PNG)
"""

import argparse
import csv
import os
import subprocess
import sys
import tempfile

import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
RANDOM_PLAN = os.path.join(SCRIPT_DIR, "random_plan.py")


def parse_int_list(s):
    return [int(x) for x in s.split(",") if x.strip()]


def generate_plan(seed, cores, args, out_path):
    cmd = [sys.executable, RANDOM_PLAN,
           "--seed", str(seed), "--platform", args.platform,
           "--benchmarks", args.benchmarks,
           "--tasks", str(args.tasks),
           "--periods", args.periods,
           "--min-subtasks", str(args.min_subtasks),
           "--max-subtasks", str(args.max_subtasks),
           "--cores", str(cores),
           "--strategy", args.strategy,
           "--sort-by", args.sort_by,
           "--weight", args.weight,
           "-o", out_path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"random_plan.py failed (seed={seed}, cores={cores}):\n{p.stderr}")


def is_schedulable(plan_path, allocate_bin):
    p = subprocess.run([allocate_bin, plan_path], capture_output=True, text=True)
    return p.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", default="x86")
    ap.add_argument("--benchmarks", default="cover,prime,whet",
                     help="comma-separated allowlist passed to random_plan.py")
    ap.add_argument("--tasks", type=int, default=32)
    ap.add_argument("--min-subtasks", type=int, default=2)
    ap.add_argument("--max-subtasks", type=int, default=5)
    ap.add_argument("--periods", default="1,2,3,4,6,12", help="ms grid, CSV")
    ap.add_argument("--strategy", default="worst_fit",
                     choices=["first_fit", "best_fit", "worst_fit"])
    ap.add_argument("--sort-by", default="remaining_utilization_desc")
    ap.add_argument("--weight", default="utilization", choices=["count", "utilization"])
    ap.add_argument("--cores", default="1,2,3,4,5,6,7,8,10,12,16,20,24",
                     help="comma-separated core counts to sweep (the x axis)")
    ap.add_argument("--seeds", type=int, default=100,
                     help="independent random tasksets tested per core count")
    ap.add_argument("--seed-start", type=int, default=0)
    ap.add_argument("--allocate-bin", default=os.path.join(REPO_ROOT, "allocate"))
    ap.add_argument("--label", default=None,
                     help="legend label; defaults to '<strategy> + <sort_by>'")
    ap.add_argument("--csv", default=os.path.join(REPO_ROOT, "results",
                                                    "taskset_scheduled.csv"))
    ap.add_argument("-o", "--output", default=os.path.join(REPO_ROOT, "results",
                                                             "taskset_scheduled.png"))
    args = ap.parse_args()

    if not os.path.isfile(args.allocate_bin) or not os.access(args.allocate_bin, os.X_OK):
        sys.exit(f"{args.allocate_bin} not found or not executable. "
                 f"Build it first: make allocate")

    cores_list = parse_int_list(args.cores)
    seeds = list(range(args.seed_start, args.seed_start + args.seeds))
    label = args.label or f"{args.strategy} + {args.sort_by}"

    results = []  # (cores, ok, total)
    with tempfile.TemporaryDirectory(prefix="taskset_sweep_") as tmpdir:
        for cores in cores_list:
            ok = 0
            for seed in seeds:
                plan_path = os.path.join(tmpdir, f"plan_c{cores}_s{seed}.json")
                generate_plan(seed, cores, args, plan_path)
                if is_schedulable(plan_path, args.allocate_bin):
                    ok += 1
            results.append((cores, ok, len(seeds)))
            print(f"cores={cores:3d}  {ok:4d}/{len(seeds)} tasksets scheduled")

    os.makedirs(os.path.dirname(args.csv) or ".", exist_ok=True)
    with open(args.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cores", "scheduled", "total", "strategy", "sort_by",
                    "platform", "benchmarks", "tasks"])
        for cores, ok, total in results:
            w.writerow([cores, ok, total, args.strategy, args.sort_by,
                        args.platform, args.benchmarks, args.tasks])
    print(f"wrote {args.csv}")

    fig, ax = plt.subplots(figsize=(7, 5))
    xs = [r[0] for r in results]
    ys = [r[1] for r in results]
    ax.plot(xs, ys, marker="o", linestyle="-", color="tab:blue", label=label)
    ax.set_xlabel("Number of cores")
    ax.set_ylabel(f"Taskset scheduled (out of {len(seeds)})")
    ax.set_ylim(0, len(seeds) * 1.05)
    ax.legend()
    fig.tight_layout()
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    fig.savefig(args.output, dpi=150)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
