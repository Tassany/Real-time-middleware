#!/usr/bin/env python3
"""
random_plan.py — generates a random deployment plan from the measured WCETs of
the Malardalen benchmarks.

Usage:
    python3 scripts/random_plan.py --tasks 32 --seed 1 -o plan_random.json
    python3 scripts/random_plan.py --tasks 54 --min-subtasks 3 --max-subtasks 3
    python3 scripts/random_plan.py --platform pi4 --cores 3 --strategy worst_fit

Every task is a linear chain source -> intermediate* -> sink whose length is
drawn from [--min-subtasks, --max-subtasks]. The period is drawn from the
--periods grid, the benchmark of each subtask is drawn from --benchmarks, and
wcet_ns comes from the table of the selected platform. Priorities are
rate-monotonic: the shortest period gets --prio-high, the longest --prio-low.

The load is not a knob. You ask for N tasks and the script reports the
utilization that came out, the same way allocate would.

`core` is deliberately left out of every subtask, so the plan reaches
allocator::apply_auto_allocation with CORE_UNASSIGNED and gets packed by the
strategy declared in the allocation block.

Stdlib only, so it runs on a bare Raspberry Pi.
"""

import argparse
import collections
import json
import math
import random
import sys

MS = 1_000_000

# wcet_bench/cycle_counter/RELATORIO.md sec. 4 (mean column), Raspberry Pi 5,
# gcc -std=gnu89 -O0 -fno-builtin -fno-stack-protector, performance governor.
# Rebuilding the benchmark objects at another -O level invalidates these.
#
# The first six of the pi4 column were taken from
# plans/deployment_plan_lowsat_pi04.json (commit cd09f92); there is no table
# for it in RELATORIO.md. The other 26 are docs/tabelas-heuristicas.tex
# tab:obs_wcet_pi4 (Time Mean column, us -> ns, exact since the source has 3
# decimals), measured on the same Raspberry Pi 4 board.
#
# pi5 stays at six entries: there is no Pi 5 measurement anywhere in the repo
# for the other 26, so --platform pi5 cannot use them.
WCET_NS = {
    "pi5": {"matmult": 48050, "bsort100": 30310, "crc": 821,
            "ud": 789, "fft1": 746, "statemate": 207},
    "pi4": {
        "matmult": 94963, "bsort100": 73833, "crc": 1259,
        "ud": 2056, "fft1": 1574, "statemate": 389,
        "whet": 620381, "lms": 241010, "fir": 150057, "adpcm": 122303,
        "edn": 43019, "ndes": 27269, "ns": 6339, "cover": 3783,
        "prime": 3358, "nsichneu": 2412, "ludcmp": 2159, "minver": 1940,
        "cnt": 1896, "compress": 1867, "jfdctint": 1768, "fdct": 1546,
        "expint": 1366, "recursion": 1117, "duff": 927, "qurt": 691,
        "insertsort": 597, "fibcall": 209, "fac": 175,
        "janne_complex": 140, "lcdnum": 131, "bs": 106,
    },
}

# The period grid every hand-written plan in the repo uses.
DEFAULT_PERIODS_MS = [1, 2, 3, 4, 6, 12]

# Ignored by parser_json.cpp, but present in every plan and read by
# wcet_bench/tools/codegen.cpp, so it stays.
OUT = {"source": "double", "intermediate": "double", "sink": "void"}


def priority_map(periods_ms, hi, lo):
    """period_ms -> priority, spread linearly from hi (shortest) to lo (longest)."""
    uniq = sorted(set(periods_ms))
    if len(uniq) == 1:
        return {uniq[0]: hi}
    step = (hi - lo) / (len(uniq) - 1)
    return {p: int(round(hi - i * step)) for i, p in enumerate(uniq)}


def chain_roles(n):
    """Component types of a chain of n subtasks, first source and last sink."""
    return ["source"] + ["intermediate"] * (n - 2) + ["sink"]


def build_plan(rng, args):
    """Returns (plan_dict, stats_dict). Pure: does not touch the filesystem."""
    wcet = WCET_NS[args.platform]
    prio = priority_map(args.periods, args.prio_high, args.prio_low)

    tasks, conns = [], []
    sid = tid = 1

    for _ in range(args.tasks):
        period_ms = rng.choice(args.periods)
        n = rng.randint(args.min_subtasks, args.max_subtasks)
        first = sid

        subtasks = []
        for kind in chain_roles(n):
            name = rng.choice(args.benchmarks)
            subtasks.append({
                "id": sid,
                "component_type": kind,
                "priority": prio[period_ms],
                "period_ns": period_ms * MS,
                "deadline_ns": period_ms * MS,
                "output_type": OUT[kind],
                "config": {},
                "wcet_ns": wcet[name],
                "benchmark": name,
            })
            sid += 1

        for i in range(n - 1):
            conns.append({"upstream": first + i, "downstream": first + i + 1})

        tasks.append({"id": tid, "subtasks": subtasks})
        tid += 1

    plan = {
        "hosts": [{"name": "localhost", "address": "127.0.0.1"}],
        "allocation": {
            "strategy": args.strategy,
            "sort_by": args.sort_by,
            "weight": args.weight,
            "num_cores": args.cores,
            "capacity": args.capacity,
        },
        "tasks": tasks,
        "connections": conns,
    }
    return plan, plan_stats(tasks, conns, args.cores)


def plan_stats(tasks, conns, num_cores):
    """The load figures the allocator and evaluation actually react to."""
    all_st = [s for t in tasks for s in t["subtasks"]]

    util = sum(s["wcet_ns"] / s["period_ns"] for s in all_st)
    disp = sum(1.0 / (s["period_ns"] / MS) for s in all_st)
    per_core = disp / num_cores
    slack_us = 1000.0 * (1.0 - util / num_cores)

    return {
        "tasks": len(tasks),
        "subtasks": len(all_st),
        "edges": len(conns),
        "util_total": util,
        "util_per_core": util / num_cores,
        "dispatches_ms": disp,
        # Overhead each job may cost before the cores stop keeping up. Negative
        # means the declared work alone already exceeds capacity.
        "overhead_budget_us": slack_us / per_core if per_core else float("inf"),
        "by_benchmark": collections.Counter(s["benchmark"] for s in all_st),
        "by_period_ms": collections.Counter(s["period_ns"] // MS for s in all_st),
        "by_length": collections.Counter(len(t["subtasks"]) for t in tasks),
    }


def feasibility_warning(st, args):
    """Message if apply_auto_allocation will reject this plan, else None."""
    if args.weight == "utilization":
        cap = args.capacity if args.capacity > 0 else 1.0
        if st["util_per_core"] > cap:
            return (f"util/core {st['util_per_core']:.3f} exceeds the capacity "
                    f"{cap:g} of the utilization weight; "
                    f"allocator::apply_auto_allocation will throw "
                    f"'automatic allocation infeasible on {args.cores} cores'")
    else:
        # weight=count packs subtasks per core, capacity 0 derives ceil(n/cores).
        cap = (args.capacity if args.capacity > 0
               else math.ceil(st["subtasks"] / args.cores))
        if st["subtasks"] > cap * args.cores:
            return (f"{st['subtasks']} subtasks do not fit in {args.cores} cores "
                    f"at capacity {cap:g} under the count weight")
    return None


def main():
    ap = argparse.ArgumentParser(
        description="Generates a random deployment plan.")
    ap.add_argument("--tasks", type=int, default=32,
                    help="number of tasks to generate (default: 32)")
    ap.add_argument("--min-subtasks", type=int, default=2,
                    help="shortest chain, at least 2 (default: 2)")
    ap.add_argument("--max-subtasks", type=int, default=5,
                    help="longest chain (default: 5)")
    ap.add_argument("--periods", default=",".join(str(p) for p in DEFAULT_PERIODS_MS),
                    help="comma-separated period grid in ms (default: 1,2,3,4,6,12)")
    ap.add_argument("--benchmarks", default=None,
                    help="comma-separated subset to draw from (default: all six)")
    ap.add_argument("--platform", choices=sorted(WCET_NS), default="pi5",
                    help="which measured wcet_ns table to use (default: pi5)")
    ap.add_argument("--cores", type=int, default=3,
                    help="allocation.num_cores (default: 3)")
    ap.add_argument("--strategy", default="first_fit",
                    choices=["first_fit", "best_fit", "worst_fit"])
    ap.add_argument("--sort-by", default="none",
                    choices=["priority_desc", "priority_asc", "period_asc",
                             "period_desc", "utilization_asc", "utilization_desc",
                             "remaining_utilization_desc", "none"])
    ap.add_argument("--weight", default="utilization",
                    choices=["count", "utilization"])
    ap.add_argument("--capacity", type=float, default=0.0,
                    help="allocation.capacity, 0 derives from the weight mode")
    ap.add_argument("--prio-high", type=int, default=90,
                    help="priority of the shortest period (default: 90)")
    ap.add_argument("--prio-low", type=int, default=40,
                    help="priority of the longest period (default: 40)")
    ap.add_argument("--seed", type=int, default=None,
                    help="RNG seed; same seed gives a byte-identical plan")
    ap.add_argument("-o", "--output", default="plans/deployment_plan_random.json",
                    help="destination file (default: plans/deployment_plan_random.json)")
    args = ap.parse_args()

    # Validation happens before anything is written, so a bad flag never leaves
    # a half-written plan behind.
    if args.tasks <= 0:
        sys.exit("--tasks must be greater than 0")
    if args.min_subtasks < 2:
        sys.exit("--min-subtasks must be at least 2; a lone source has no sink, "
                 "and a sink with no predecessor makes preds.at(id) throw in "
                 "execute_plan.cpp and evaluation.cpp")
    if args.max_subtasks < args.min_subtasks:
        sys.exit("--max-subtasks must not be smaller than --min-subtasks")

    try:
        args.periods = [int(p) for p in args.periods.split(",") if p.strip()]
    except ValueError:
        sys.exit(f"--periods: not a comma-separated list of integers: {args.periods}")
    if not args.periods:
        sys.exit("--periods is empty")
    if any(p <= 0 for p in args.periods):
        sys.exit("--periods: every period must be greater than 0 ms")

    known = WCET_NS[args.platform]
    if args.benchmarks is None:
        args.benchmarks = sorted(known)
    else:
        args.benchmarks = [b.strip() for b in args.benchmarks.split(",") if b.strip()]
        unknown = [b for b in args.benchmarks if b not in known]
        if unknown:
            sys.exit(f"--benchmarks: unknown name(s) {', '.join(unknown)} "
                     f"(expected {', '.join(sorted(known))})")
    if not args.benchmarks:
        sys.exit("--benchmarks is empty")

    if args.prio_high < args.prio_low:
        sys.exit("--prio-high must not be smaller than --prio-low")
    if args.cores <= 0:
        sys.exit("--cores must be greater than 0")

    rng = random.Random(args.seed)
    plan, st = build_plan(rng, args)

    with open(args.output, "w") as f:
        json.dump(plan, f, indent=2)
        f.write("\n")

    prio = priority_map(args.periods, args.prio_high, args.prio_low)

    print(f"Wrote {args.output}")
    print(f"  platform         {args.platform}")
    print(f"  seed             {args.seed if args.seed is not None else '(none)'}")
    print(f"  tasks            {st['tasks']}")
    print(f"  subtasks         {st['subtasks']}")
    print(f"  edges            {st['edges']}")
    print(f"  util total       {st['util_total']:.3f}")
    print(f"  util per core    {st['util_per_core']:.3f}  ({args.cores} cores)")
    print(f"  dispatches/ms    {st['dispatches_ms']:.1f}")
    print(f"  overhead budget  {st['overhead_budget_us']:.1f} us/job")

    print("\n  chain length  tasks")
    for n in sorted(st["by_length"]):
        print(f"  {n:>12} {st['by_length'][n]:>6}")

    print("\n  period_ms  prio  subtasks")
    for p in sorted(st["by_period_ms"]):
        print(f"  {p:>9} {prio[p]:>5} {st['by_period_ms'][p]:>9}")

    print("\n  benchmark   wcet_us  subtasks")
    for name in sorted(st["by_benchmark"]):
        print(f"  {name:<10} {known[name] / 1000.0:>8.3f} {st['by_benchmark'][name]:>9}")

    warn = feasibility_warning(st, args)
    if warn:
        # Not an error: an oversaturated plan is a legitimate thing to generate,
        # plans/deployment_plan_oversat.json sits at util/core 1.094 on purpose.
        print(f"\nWARNING: {warn}", file=sys.stderr)


if __name__ == "__main__":
    main()
