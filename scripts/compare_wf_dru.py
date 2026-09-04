#!/usr/bin/env python3
"""
compare_wf_dru.py — compares worst_fit+none against worst_fit+DRU
(Decreasing Remaining Utilization, Verucchi et al. 2023 Sect. 4) on real
`evaluation` runs, across a handful of random seeds.

Usage (run on the board, as root so evaluation gets SCHED_FIFO):
    sudo python3 scripts/compare_wf_dru.py
    sudo python3 scripts/compare_wf_dru.py --seeds 10 --hyperperiods 100

For each seed, generates ONE base plan via random_plan.py (all 32 measured
Malardalen benchmarks, --platform pi4 by default), then clones it into two
variants that differ in exactly one JSON field (allocation.sort_by) — so
config A and config B always see the same DAG/benchmarks/WCETs for a given
seed, and any difference in the results is attributable to the sort
criterion alone, not to a different random task set.

Output goes to --outdir:
    seed<N>_{none,dru}.json       the two plan variants for that seed
    seed<N>_{none,dru}.csv        evaluation's raw latency samples
    summary.csv                   one row per (seed, config)

Default --periods (10-120ms) is deliberately longer than random_plan.py's own
default (1-12ms): at --tasks 32 the short grid drives ~40 dispatches/ms,
which is more than the measured dispatch overhead (~65-70us median per
release, see the Fase 2 overhead pilot) can absorb even before any real
benchmark WCET is spent — every config ends up dominated by dispatch
saturation, not by the sort criterion being compared. Confirmed empirically:
the first full run at the short grid gave a mixed, seed-dependent result
(DRU better in 3 of 5 seeds, worse in 2, one large outlier) at 7-47% miss
rates despite declared utilization of only 0.5-0.7 per core.

Stdlib only, so it runs on a bare Raspberry Pi.
"""

import argparse
import copy
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RANDOM_PLAN = os.path.join(SCRIPT_DIR, "random_plan.py")

CONFIGS = [
    ("none", "none"),
    ("dru",  "remaining_utilization_desc"),
]

SUMMARY_RE = re.compile(
    r"Total jobs:\s*(\d+)\s+Deadline misses:\s*(\d+)\s+Miss rate:\s*([\d.]+)%")


def generate_base_plan(seed, args, out_path):
    cmd = [sys.executable, RANDOM_PLAN,
           "--seed", str(seed), "--platform", args.platform,
           "--tasks", str(args.tasks),
           "--periods", args.periods,
           "--min-subtasks", str(args.min_subtasks),
           "--max-subtasks", str(args.max_subtasks),
           "--cores", str(args.cores),
           "--strategy", "worst_fit", "--sort-by", "none",
           "-o", out_path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"random_plan.py failed for seed {seed}:\n{p.stderr}")


def write_variant(base_plan, sort_by, out_path):
    variant = copy.deepcopy(base_plan)
    variant["allocation"]["sort_by"] = sort_by
    with open(out_path, "w") as f:
        json.dump(variant, f, indent=2)


def run_evaluation(binary, plan_path, hyperperiods, timeout):
    """Runs evaluation. Returns (summary_dict, error_string_or_None)."""
    try:
        p = subprocess.run([binary, plan_path, str(hyperperiods)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {}, f"timeout after {timeout}s"

    if p.returncode != 0:
        msg = (p.stderr or p.stdout).strip().splitlines()
        return {}, (msg[-1] if msg else f"exit {p.returncode}")

    m = SUMMARY_RE.search(p.stdout)
    if not m:
        return {}, "could not parse the summary line"

    return {"jobs": int(m.group(1)),
            "misses": int(m.group(2)),
            "miss_pct": float(m.group(3))}, None


def sample_stats(path):
    """mean / median / max latency and peak-to-peak jitter from a latency CSV."""
    values = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            values.append(float(row["latency_us"]))
    if not values:
        return {}
    return {"lat_mean_us":   sum(values) / len(values),
            "lat_median_us": statistics.median(values),
            "lat_max_us":    max(values),
            "jitter_us":     max(values) - min(values)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=5, help="number of seeds to run")
    ap.add_argument("--seed-start", type=int, default=1)
    ap.add_argument("--platform", default="pi4", choices=["pi4", "pi5"])
    ap.add_argument("--tasks", type=int, default=32)
    ap.add_argument("--periods", default="10,20,30,40,60,120",
                    help="comma-separated period grid in ms, passed through to "
                         "random_plan.py. Longer than random_plan.py's own "
                         "default (1,2,3,4,6,12): at 32 tasks that grid drives "
                         "~40 dispatches/ms, well past what the measured "
                         "dispatch overhead (~65-70us median, see the Fase 2 "
                         "pilot) can absorb, so the comparison ends up "
                         "dominated by dispatch saturation instead of the "
                         "sort criterion.")
    ap.add_argument("--min-subtasks", type=int, default=2)
    ap.add_argument("--max-subtasks", type=int, default=5)
    ap.add_argument("--cores", type=int, default=3)
    ap.add_argument("--hyperperiods", type=int, default=50)
    ap.add_argument("--binary", default="./evaluation")
    ap.add_argument("--outdir", default="results/wf_dru_comparison")
    ap.add_argument("--timeout", type=int, default=300,
                    help="per-run timeout in seconds")
    args = ap.parse_args()

    if not os.access(args.binary, os.X_OK):
        sys.exit(f"{args.binary} not found or not executable; run `make evaluation` first")

    if os.geteuid() != 0:
        print("WARNING: not running as root, so evaluation cannot apply "
              "SCHED_FIFO and every number below will be dominated by CFS "
              "scheduling noise.\n", file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)

    cols = ["seed", "config", "jobs", "misses", "miss_pct",
            "lat_mean_us", "lat_median_us", "lat_max_us", "jitter_us", "error"]
    rows = []

    hdr = (f"{'seed':>6} {'config':>8} {'jobs':>7} {'misses':>7} {'miss%':>7} "
           f"{'mean_us':>10} {'median_us':>10} {'max_us':>10}")
    print(hdr)
    print("-" * len(hdr))

    for seed in range(args.seed_start, args.seed_start + args.seeds):
        base_path = os.path.join(args.outdir, f"seed{seed}_base.json")
        generate_base_plan(seed, args, base_path)
        with open(base_path) as f:
            base_plan = json.load(f)

        for tag, sort_by in CONFIGS:
            plan_path = os.path.join(args.outdir, f"seed{seed}_{tag}.json")
            write_variant(base_plan, sort_by, plan_path)

            summary, err = run_evaluation(args.binary, plan_path,
                                          args.hyperperiods, args.timeout)
            row = {"seed": seed, "config": tag, "error": err or "", **summary}

            if not err and os.path.exists("latency_samples.csv"):
                dest = os.path.join(args.outdir, f"seed{seed}_{tag}.csv")
                shutil.move("latency_samples.csv", dest)
                row.update(sample_stats(dest))

            rows.append(row)

            if err:
                print(f"{seed:>6} {tag:>8}   -> {err}")
            else:
                print(f"{seed:>6} {tag:>8} {row['jobs']:>7} {row['misses']:>7} "
                      f"{row['miss_pct']:>7.2f} {row.get('lat_mean_us', 0):>10.1f} "
                      f"{row.get('lat_median_us', 0):>10.1f} {row.get('lat_max_us', 0):>10.1f}")

        # Written every seed so an interrupted run still leaves usable data.
        summary_path = os.path.join(args.outdir, "summary.csv")
        with open(summary_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)

    print(f"\nWrote {os.path.join(args.outdir, 'summary.csv')}")

    # none vs dru diff per seed (dru - none): negative miss_pct/lat_mean_us
    # means DRU did better.
    by_seed = {}
    for r in rows:
        if r["error"]:
            continue
        by_seed.setdefault(r["seed"], {})[r["config"]] = r

    diffs = []
    print(f"\n{'seed':>6} {'d_miss_pct':>12} {'d_lat_mean_us':>15}")
    for seed, cfgs in sorted(by_seed.items()):
        if "none" not in cfgs or "dru" not in cfgs:
            continue
        d_miss = cfgs["dru"]["miss_pct"] - cfgs["none"]["miss_pct"]
        d_lat  = cfgs["dru"].get("lat_mean_us", 0) - cfgs["none"].get("lat_mean_us", 0)
        diffs.append((d_miss, d_lat))
        print(f"{seed:>6} {d_miss:>12.2f} {d_lat:>15.1f}")

    if diffs:
        mean_d_miss = sum(d[0] for d in diffs) / len(diffs)
        mean_d_lat  = sum(d[1] for d in diffs) / len(diffs)
        print("-" * 36)
        print(f"{'mean':>6} {mean_d_miss:>12.2f} {mean_d_lat:>15.1f}")


if __name__ == "__main__":
    main()
