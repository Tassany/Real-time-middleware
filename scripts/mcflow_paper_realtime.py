#!/usr/bin/env python3
"""
mcflow_paper_realtime.py — builds the deployment plan for the "Real-time
Performance" experiment (Huang et al. 2012, MCFlow, Sect. VI-C, Table I).

Three tasks (High/Medium/Low), each shaped Ts -> {T0,T1,T2,T3} -> Tm (a
split-parallel-merge DAG: Ts fans out to 4 parallel subtasks, which fan back
into Tm). All subtasks of a task share that task's priority; the paper's own
invariant is that every High subtask outranks every Medium one, which in
turn outranks every Low one. Table I pins each subtask to a specific core;
some of a task's own subtasks intentionally share a core (e.g. High's Ts,
T0 and Tm are all core 0), which also makes them share one Dispatcher
thread — that is the paper's setup, not an artifact of this script.

High and Medium are fixed at 200 Hz / 100 Hz per the paper. Low is the
variable being swept (Table II/III use 50, 60, 70, 80, 90 Hz); pass
--low-hz to pick the point.

Workload is in microseconds (paper's own unit), realized by the synthetic
busy900/1800/3600/4500 entries in src/bench_registry.cpp — plain CPU spins,
not Malardalen benchmarks, added specifically because these durations don't
correspond to any real benchmark's measured WCET. A workload of 0 (Medium's
T0) means no benchmark at all, matching the paper's "no extra workload
generated in those two subtasks" for that cell.

Usage:
    python3 scripts/mcflow_paper_realtime.py --low-hz 50
    python3 scripts/mcflow_paper_realtime.py --low-hz 90 -o plans/mcflow_paper_90hz.json
"""

import argparse
import collections
import json
import sys

US = 1_000  # microseconds -> nanoseconds

# workload in microseconds -> busyN benchmark name; 0 means no body at all.
BUSY = {900: "busy900", 1800: "busy1800", 3600: "busy3600", 4500: "busy4500"}

# (core, workload_us) per subtask, exactly Table I (Huang et al. 2012, p.111).
TABLE = {
    "High":   {"hz": 200,  "priority": 90, "subtasks": {
        "Ts": (0, 900), "T0": (0, 1800), "T1": (1, 1800), "T2": (2, 1800),
        "T3": (3, 1800), "Tm": (0, 900),
    }},
    "Medium": {"hz": 100,  "priority": 60, "subtasks": {
        "Ts": (1, 900), "T0": (0, 0),    "T1": (1, 1800), "T2": (2, 1800),
        "T3": (3, 1800), "Tm": (1, 1800),
    }},
    "Low":    {"hz": None, "priority": 30, "subtasks": {
        "Ts": (2, 900), "T0": (0, 1800), "T1": (1, 900),  "T2": (2, 4500),
        "T3": (3, 3600), "Tm": (2, 900),
    }},
}

ROLE = {"Ts": "source", "T0": "intermediate", "T1": "intermediate",
        "T2": "intermediate", "T3": "intermediate", "Tm": "sink"}

# Ts fans out to T0..T3; all four fan back into Tm (Figure 8's split/merge).
EDGES = [("Ts", "T0"), ("Ts", "T1"), ("Ts", "T2"), ("Ts", "T3"),
         ("T0", "Tm"), ("T1", "Tm"), ("T2", "Tm"), ("T3", "Tm")]


def build_plan(low_hz):
    tasks, conns = [], []
    sid = tid = 1

    for name, spec in TABLE.items():
        hz = low_hz if spec["hz"] is None else spec["hz"]
        period_ns = round(1e9 / hz)
        prio = spec["priority"]

        ids = {}
        subtasks = []
        for label in ("Ts", "T0", "T1", "T2", "T3", "Tm"):
            core, us = spec["subtasks"][label]
            ids[label] = sid
            subtasks.append({
                "id": sid,
                "component_type": ROLE[label],
                "core": core,
                "priority": prio,
                "period_ns": period_ns,
                "deadline_ns": period_ns,  # implicit deadline, per the paper
                "output_type": "double" if ROLE[label] != "sink" else "void",
                "config": {},
                "wcet_ns": us * US,
                "benchmark": BUSY.get(us, ""),
            })
            sid += 1

        for up, down in EDGES:
            conns.append({"upstream": ids[up], "downstream": ids[down]})

        tasks.append({"id": tid, "name": name, "subtasks": subtasks})
        tid += 1

    plan = {
        "hosts": [{"name": "localhost", "address": "127.0.0.1"}],
        "allocation": {"strategy": "first_fit", "sort_by": "none",
                       "weight": "count", "num_cores": 4, "capacity": 0},
        "tasks": [{"id": t["id"], "subtasks": t["subtasks"]} for t in tasks],
        "connections": conns,
    }
    return plan, tasks


def summarize(tasks):
    util_per_core = collections.defaultdict(float)
    for t in tasks:
        period_ns = t["subtasks"][0]["period_ns"]
        for s in t["subtasks"]:
            util_per_core[s["core"]] += s["wcet_ns"] / period_ns

    print("\n  task    hz     priority  core:workload_us")
    for t in tasks:
        hz = round(1e9 / t["subtasks"][0]["period_ns"])
        prio = t["subtasks"][0]["priority"]
        by_label = ", ".join(
            f"{label}=c{spec[0]}:{spec[1]}"
            for label, spec in TABLE[t["name"]]["subtasks"].items())
        print(f"  {t['name']:<7} {hz:<6} {prio:<9} {by_label}")

    print("\n  core  utilization (sum of wcet/period across all 3 tasks)")
    for core in sorted(util_per_core):
        flag = "  <-- oversaturated" if util_per_core[core] > 1.0 else ""
        print(f"  {core:<5} {util_per_core[core]:.3f}{flag}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--low-hz", type=float, default=50.0,
                     help="frequency of the Low-priority task in Hz "
                          "(paper sweeps 50,60,70,80,90; default: 50)")
    ap.add_argument("-o", "--output", default=None,
                     help="destination file (default: "
                          "plans/mcflow_paper_<low_hz>hz.json)")
    args = ap.parse_args()

    if args.low_hz <= 0:
        sys.exit("--low-hz must be greater than 0")

    plan, tasks = build_plan(args.low_hz)

    out = args.output or f"plans/mcflow_paper_{args.low_hz:g}hz.json"
    with open(out, "w") as f:
        json.dump(plan, f, indent=2)
        f.write("\n")

    print(f"Wrote {out}")
    print(f"  Low task: {args.low_hz:g} Hz "
          f"(period {round(1e9 / args.low_hz)} ns)")
    summarize(tasks)


if __name__ == "__main__":
    main()
