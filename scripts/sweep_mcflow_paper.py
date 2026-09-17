#!/usr/bin/env python3
"""
sweep_mcflow_paper.py — runs the Section VI-C ("Real-time Performance")
experiment at Low = 50, 60, 70, 80, 90 Hz and compares this project's deadline
miss ratio and average end-to-end response time against the values Huang et
al. 2012 (MCFlow) report in Tables II and III for the same experiment.

Needs root for a fair comparison (SCHED_FIFO):
    sudo python3 scripts/sweep_mcflow_paper.py
    sudo python3 scripts/sweep_mcflow_paper.py --duration-ms 20000

Without sudo, evaluation_precise prints a warning per dispatcher and runs
under the plain Linux scheduler instead of SCHED_FIFO — useful as a contrast
(see the priority-preemption test earlier in this project's history), not as
the real comparison point.

Runs scripts/evaluation_precise (not evaluation.cpp's own tick loop): that
loop drives every source off one shared tick counter, checking
`tick % (period / min_p) == 0`, which is only exact when every source period
is a whole multiple of the tick. 1/60, 1/70, 1/90 Hz are not — e.g. 1/70Hz is
14.2857ms against a 5ms tick, truncating to a ratio of 2 and silently firing
that source every 10ms instead, 30% more often than declared (genuinely more
load, not just a measurement artifact — this is what made the first sweep
attempt look catastrophic for classes that should have been protected).
evaluation_precise gives each source its own exact next-fire time instead,
so --duration-ms is a plain wall-clock budget with no LCM/rounding involved.

Requires the `evaluation_precise` binary already built
(`make evaluation_precise`).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcflow_paper_realtime as paper  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOW_HZ_POINTS = [50, 60, 70, 80, 90]

# task_id -> class name, per the generation order in mcflow_paper_realtime.py
# (TABLE is High, Medium, Low, and build_plan() assigns task ids 1, 2, 3 in
# that same iteration order).
TASK_NAME = {1: "High", 2: "Medium", 3: "Low"}

# Huang et al. 2012, Table II (deadline miss ratio) and Table III (average
# response time, us), transcribed as-is for side-by-side comparison.
PAPER_MISS_RATIO = {
    50: {"High": 0.0,  "Medium": 0.0,  "Low": 0.0},
    60: {"High": 0.0,  "Medium": 0.0,  "Low": 0.0},
    70: {"High": 0.0,  "Medium": 0.0,  "Low": 0.06},
    80: {"High": 0.0,  "Medium": 0.0,  "Low": 0.75},
    90: {"High": 0.0,  "Medium": 0.0,  "Low": 1.0},
}
PAPER_RESPONSE_US = {
    50: {"High": 3918, "Medium": 8127, "Low": 14918},
    60: {"High": 3929, "Medium": 8065, "Low": 12615},
    70: {"High": 3926, "Medium": 8063, "Low": 12881},
    80: {"High": 3931, "Medium": 8129, "Low": 13574},
    90: {"High": 3928, "Medium": 8045, "Low": 18919},
}

TASK_TABLE_RE = re.compile(
    r"=== End-to-end Response Time per Task ===\n"
    r".*\n-+\n"
    r"((?:\d+\s+\d+\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+\d+\n?)+)"
)


def parse_task_table(stdout):
    """Returns {task_id: (jobs, resp_mean_us, misses)} from evaluation's output."""
    m = TASK_TABLE_RE.search(stdout)
    if not m:
        sys.exit("could not find the 'End-to-end Response Time per Task' "
                  "table in evaluation's output:\n" + stdout)
    result = {}
    for line in m.group(1).splitlines():
        task_id, jobs, resp_min, resp_mean, resp_max, misses = line.split()
        result[int(task_id)] = (int(jobs), float(resp_mean), int(misses))
    return result


def run_point(low_hz, evaluate_bin, duration_ms, tmpdir, isolate_tm=False):
    plan, _ = paper.build_plan(low_hz, isolate_tm=isolate_tm)
    plan_path = os.path.join(tmpdir, f"plan_{low_hz}hz.json")
    with open(plan_path, "w") as f:
        json.dump(plan, f)

    p = subprocess.run([evaluate_bin, plan_path, str(duration_ms)],
                       capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"evaluation_precise failed for {low_hz}Hz:\n{p.stderr}")

    by_task = parse_task_table(p.stdout)
    return {TASK_NAME[tid]: {"jobs": jobs, "resp_us": resp_mean,
                              "miss_ratio": (misses / jobs if jobs else 0.0)}
            for tid, (jobs, resp_mean, misses) in by_task.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--evaluate-bin",
                     default=os.path.join(REPO_ROOT, "evaluation_precise"))
    ap.add_argument("--duration-ms", type=int, default=10000,
                     help="wall-clock run length per point, same for all "
                          "five so the sweep is a fair comparison (default: "
                          "10000 = 10s per point, 50s total)")
    ap.add_argument("--isolate-tm", action="store_true",
                     help="build each point's plan with mcflow_paper_realtime's "
                          "--isolate-tm layout instead of Table I's own")
    args = ap.parse_args()

    if not os.path.isfile(args.evaluate_bin) or not os.access(args.evaluate_bin, os.X_OK):
        sys.exit(f"{args.evaluate_bin} not found or not executable. "
                 f"Build it first: make evaluation_precise")
    if os.geteuid() != 0:
        print("WARNING: not running as root — evaluation will fall back to the "
              "plain Linux scheduler (no SCHED_FIFO), so these numbers are not "
              "the real comparison point, only a smoke test. Re-run with sudo.\n",
              file=sys.stderr)

    results = {}
    with tempfile.TemporaryDirectory(prefix="mcflow_sweep_") as tmpdir:
        for hz in LOW_HZ_POINTS:
            print(f"running Low={hz}Hz ...", file=sys.stderr)
            results[hz] = run_point(hz, args.evaluate_bin, args.duration_ms,
                                     tmpdir, isolate_tm=args.isolate_tm)

    # --- Deadline miss ratio, side by side with Table II ---
    print("\n=== Deadline miss ratio (yours vs. Huang et al. 2012, Table II) ===")
    header = f"{'Hz':>4}  " + "  ".join(
        f"{cls:>18}" for cls in ("High", "Medium", "Low"))
    print(header)
    for hz in LOW_HZ_POINTS:
        cells = []
        for cls in ("High", "Medium", "Low"):
            mine = results[hz][cls]["miss_ratio"]
            theirs = PAPER_MISS_RATIO[hz][cls]
            cells.append(f"{mine:.2f} (paper {theirs:.2f})".rjust(18))
        print(f"{hz:>4}  " + "  ".join(cells))

    # --- Average response time, side by side with Table III ---
    print("\n=== Average response time, us (yours vs. Huang et al. 2012, Table III) ===")
    print(header)
    for hz in LOW_HZ_POINTS:
        cells = []
        for cls in ("High", "Medium", "Low"):
            mine = results[hz][cls]["resp_us"]
            theirs = PAPER_RESPONSE_US[hz][cls]
            cells.append(f"{mine:.0f} (paper {theirs})".rjust(18))
        print(f"{hz:>4}  " + "  ".join(cells))

    print(f"\n(jobs per point: High={results[LOW_HZ_POINTS[0]]['High']['jobs']}, "
          f"Medium={results[LOW_HZ_POINTS[0]]['Medium']['jobs']}, "
          f"Low varies 11-20ms period -> ~"
          f"{results[LOW_HZ_POINTS[0]]['Low']['jobs']}-"
          f"{results[LOW_HZ_POINTS[-1]]['Low']['jobs']} jobs)")


if __name__ == "__main__":
    main()
