#!/usr/bin/env python3
"""Compare our measured observed WCET against Li et al. (2024), Figure 10.

Reads the observed_wcet.csv produced by run_all.sh and joins it against
paper_fig10.csv, emitting a Markdown table on stdout.

Standard library only, on purpose: a stock Raspberry Pi OS has python3 and no
pip packages, and the analysis should not need a network round trip.

Usage:
    ./compare.py                                # results-cold/, plus results/ if present
    ./compare.py -c results-cold -w results     # explicit
    ./compare.py -c results-cold --board firefly-cortex-a55
"""

import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Our binary names vs. the names the paper's figure uses.
#   fft1           -- the Malardalen file is fft1.c; the paper writes "fft".
#   insertsort1024 -- the paper "changed the length of the target reversed array
#                     in insertsort to 1024", so this variant is the row that
#                     measures the work they measured. The canonical 10-element
#                     insertsort maps to the same paper row but is flagged,
#                     because comparing them directly is meaningless.
PAPER_NAME = {
    "fft1": "fft",
    "insertsort1024": "insertsort",
    "insertsort": "insertsort",
}

INCOMPARABLE = {
    "insertsort": "paper sorts 1024 elements, this row sorts 10 -- see insertsort1024",
}


def load_paper(board):
    path = os.path.join(HERE, "paper_fig10.csv")
    out = {}
    with open(path, newline="") as fh:
        rows = [ln for ln in fh if not ln.startswith("#")]
    for row in csv.DictReader(rows):
        if row["board"] == board:
            out[row["benchmark"]] = {
                "estimated": float(row["estimated_cycles"]),
                "observed": float(row["observed_cycles"]),
                "ratio": float(row["ratio"]),
            }
    if not out:
        sys.exit("compare.py: no rows for board %r in paper_fig10.csv" % board)
    return out


def load_results(path):
    """Return list of per-benchmark dicts, or None if the file is not there."""
    if not path or not os.path.isfile(path):
        return None
    with open(path, newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        return None
    return rows


def num(row, key):
    """Parse a CSV cell, tolerating the empty string the harness writes when a
    column has no value (an absent PMU count is not a count of zero)."""
    v = (row.get(key) or "").strip()
    if not v:
        return None
    try:
        return float(v)
    except ValueError:
        return None


def cycles(row):
    """(mean, max, source) -- measured PMU cycles when available, else the
    cycles derived from generic-timer ticks."""
    if (row.get("pmu_ok") or "").strip() == "1":
        m, x = num(row, "mean_pmucyc"), num(row, "max_pmucyc")
        if m is not None and x is not None:
            return m, x, "PMU"
    return num(row, "mean_cycles"), num(row, "max_cycles"), "derived"


def fmt(v, nd=0):
    if v is None:
        return "n/a"
    return "{:,.{nd}f}".format(v, nd=nd)


def table(rows, paper, title, note):
    print("### %s\n" % title)
    print(note + "\n")
    print("| benchmark | n | mean cyc | **max cyc** | max/mean | paper obs cyc "
          "| mean/paper | max/paper | mean us | max us | cyc src |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--|")

    flagged = []
    for row in rows:
        name = row["benchmark"]
        mean_c, max_c, src = cycles(row)
        mean_us, max_us = num(row, "mean_us"), num(row, "max_us")
        ratio = (max_c / mean_c) if (mean_c and max_c) else None

        pname = PAPER_NAME.get(name, name)
        ref = paper.get(pname)
        incomparable = name in INCOMPARABLE
        if ref and not incomparable:
            r_mean = mean_c / ref["observed"] if mean_c else None
            r_max = max_c / ref["observed"] if max_c else None
            ref_s = fmt(ref["observed"])
        else:
            r_mean = r_max = None
            ref_s = ("%s *" % fmt(ref["observed"])) if ref else "n/a"
            if incomparable:
                flagged.append((name, INCOMPARABLE[name]))

        print("| %s | %s | %s | **%s** | %s | %s | %s | %s | %s | %s | %s |" % (
            name, row.get("n", "?"),
            fmt(mean_c), fmt(max_c),
            fmt(ratio, 2) if ratio else "n/a",
            ref_s,
            fmt(r_mean, 2) if r_mean else "--",
            fmt(r_max, 2) if r_max else "--",
            fmt(mean_us, 3), fmt(max_us, 3),
            src,
        ))
    print()
    for name, why in flagged:
        print("`*` **%s**: %s" % (name, why))
    if flagged:
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--cold", default="results-cold",
                    help="cold-cache results directory (default: results-cold)")
    ap.add_argument("-w", "--warm", default="results",
                    help="warm-cache results directory (default: results)")
    ap.add_argument("--board", default="rpi4b-cortex-a75",
                    help="which board's figure to compare against "
                         "(rpi4b-cortex-a75 = Fig. 10, firefly-cortex-a55 = Fig. 11)")
    args = ap.parse_args()

    paper = load_paper(args.board)
    cold = load_results(os.path.join(args.cold, "observed_wcet.csv"))
    warm = load_results(os.path.join(args.warm, "observed_wcet.csv"))

    if cold is None and warm is None:
        sys.exit("compare.py: no results found in %r or %r -- run ./run_all.sh first"
                 % (args.cold, args.warm))

    print("## Observed WCET: this Raspberry Pi 5 vs. Li et al. (2024), "
          "Figure 10 (Pi 4B, Cortex-A75)\n")

    if cold is not None:
        table(cold, paper, "Cold cache (`run_all.sh -f`)",
              "Caches evicted before every measured run. This is the regime static "
              "WCET analysis assumes -- an empty cache at program entry -- and so "
              "the one comparable with the paper's estimates.")
    if warm is not None:
        table(warm, paper, "Warm cache (`run_all.sh`)",
              "N back-to-back executions in one process, everything resident in L1. "
              "This is steady-state throughput, not a worst-case proxy; shown for "
              "contrast only.")

    print("""**Reading this table.** `mean` is the observed WCET exactly as Li et al.
define it, since they average five runs, and is the column to compare against
theirs. `max` is the largest execution actually seen and is the more honest
worst-case proxy; `max/mean` is the gap between the two. Neither is a sound
upper bound. Measurement can only ever report the paths that happened to
execute, so an observed WCET is a lower bound on the true worst case, which is
the whole reason static analysis exists.

The two boards differ by construction: the paper's Figure 10 is a Pi 4B
(Cortex-A75, 1.5 GHz), this is a Pi 5 (Cortex-A76, 2.4 GHz). Absolute cycle
counts are not expected to match. What should survive is the relative ordering
of the benchmarks.

`cyc src` says whether the cycle columns were counted by the PMU
(`perf_event_open`, real cycles) or derived from generic-timer ticks as
`ticks * f_cpu / f_timer`. Derived values assume the core clock held steady for
the whole batch; measured ones do not.""")


if __name__ == "__main__":
    main()
