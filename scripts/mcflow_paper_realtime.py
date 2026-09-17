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
    python3 scripts/mcflow_paper_realtime.py --low-hz 50 --isolate-tm

--isolate-tm moves each task's Tm off the 3-subtask cluster Table I puts it
in (Ts, T0 and Tm all share one core/Dispatcher thread) onto the core of one
of the T1..T3 branches instead, splitting that 3-way share into two 2-way
ones. Workload, priority and period are all unchanged — only which core Tm
runs on. This exists because Table I's own layout makes Tm both (a) gated on
the slowest of 4 parallel branches and (b) sharing a thread with Ts, the
very thing that releases the next job — once (a) delays Tm even slightly,
(b) means the next Ts queues up behind it too, and the backlog compounds
instead of draining. Moving Tm off Ts's thread breaks that specific feedback
loop without changing anything the paper's numbers are supposed to reflect.
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

# --isolate-tm: which core each task's Tm moves to instead of Ts's own.
# Chosen so no (core, priority) dispatcher ends up with more than 2
# subtasks (Table I's own layout puts 3 — Ts, T0 and Tm — on one), and so
# the resulting core loads land close to even: at 50Hz this is
# core0=0.81, core1=0.855, core2=0.81, core3=0.765 instead of Table I's
# own 0.81/0.855/0.855/0.72 concentrated onto fewer, busier dispatchers.
TM_ALT_CORE = {"High": 1, "Medium": 0, "Low": 3}


def build_plan(low_hz, isolate_tm=False):
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
            if isolate_tm and label == "Tm":
                core = TM_ALT_CORE[name]
            ids[label] = sid
            # Only Ts is a genuinely periodic release — it has no
            # predecessor, so it's the one subtask actually driven by an
            # external clock. T0..T3 and Tm are purely reactive (fan-in
            # triggered): they should run the instant their inputs are
            # ready, not be rate-limited to "once per period". Giving them
            # a nonzero period_ns activates process_subtask's release-guard
            # deferral (dispatcher.hpp) for them too — if one is ever
            # notified a hair earlier than its own period_ns clock expects
            # (routine timing jitter, not an error), it gets pushed into
            # the timer queue to wait for the lowest-priority idle thread,
            # which only runs when its whole core goes fully idle. Once
            # that happens once, the subtask's internal period_ns clock is
            # permanently out of phase with when it's actually notified, so
            # it keeps re-triggering the same deferral every cycle from
            # then on — this, not priority or capacity, is what produced
            # the runaway response times chased in this file's git history.
            subtask_period_ns = period_ns if label == "Ts" else 0
            subtasks.append({
                "id": sid,
                "component_type": ROLE[label],
                "core": core,
                "priority": prio,
                "period_ns": subtask_period_ns,
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
        labels = ("Ts", "T0", "T1", "T2", "T3", "Tm")
        by_label = ", ".join(
            f"{label}=c{s['core']}:{s['wcet_ns'] // US}"
            for label, s in zip(labels, t["subtasks"]))
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
    ap.add_argument("--isolate-tm", action="store_true",
                     help="move each task's Tm off Ts's core onto a branch "
                          "core instead (see this file's header for why)")
    args = ap.parse_args()

    if args.low_hz <= 0:
        sys.exit("--low-hz must be greater than 0")

    plan, tasks = build_plan(args.low_hz, isolate_tm=args.isolate_tm)

    suffix = "_isolatetm" if args.isolate_tm else ""
    out = args.output or f"plans/mcflow_paper_{args.low_hz:g}hz{suffix}.json"
    with open(out, "w") as f:
        json.dump(plan, f, indent=2)
        f.write("\n")

    print(f"Wrote {out}")
    print(f"  Low task: {args.low_hz:g} Hz "
          f"(period {round(1e9 / args.low_hz)} ns)")
    summarize(tasks)


if __name__ == "__main__":
    main()
