#!/usr/bin/env python3
"""
plot_latency.py — Boxplot of per-job scheduling latency from example_eval output.

Usage:
    python3 plot_latency.py [latency_samples.csv] [--max-latency US]

Produces two figures:
  latency_boxplot_by_subtask.png  — one box per subtask, coloured by period
  latency_boxplot_by_core.png     — one box per core, coloured by core

By default every sample is plotted. Pass --max-latency US to drop the tail when
it crushes the boxes, e.g. --max-latency 600. The discard is never silent: the
count is printed per core and stamped on both figures, because on a saturated
plan the tail can be a large fraction of the distribution rather than a handful
of outliers.
"""

import argparse
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

ap = argparse.ArgumentParser()
ap.add_argument("csv_path", nargs="?", default="latency_samples.csv")
ap.add_argument("--max-latency", type=float, default=None, metavar="US",
                help="drop samples with latency_us above this; omit to keep all")
args = ap.parse_args()

csv_path = args.csv_path
df = pd.read_csv(csv_path)

note = ""

if args.max_latency is not None:
    total = len(df)
    cut = df["latency_us"] > args.max_latency
    dropped = int(cut.sum())

    if dropped:
        print(f"Dropping {dropped}/{total} samples ({100 * dropped / total:.3f}%) "
              f"above {args.max_latency:g} us  (max was {df['latency_us'].max():.1f} us)")
        for c, n in df[cut].groupby("core").size().items():
            in_core = int((df["core"] == c).sum())
            print(f"  core {c}: {n}/{in_core} ({100 * n / in_core:.3f}%)")
        if dropped / total > 0.01:
            print("  WARNING: more than 1% of the samples were discarded; the tail is "
                  "part of the distribution here, not an outlier set.")
        df = df[~cut]
        note = (f"{dropped}/{total} samples ({100 * dropped / total:.2f}%) "
                f"above {args.max_latency:g} us discarded")
    else:
        print(f"No sample above {args.max_latency:g} us; nothing discarded.")

    if df.empty:
        raise SystemExit(f"No sample left below {args.max_latency:g} us.")

palette = plt.cm.tab10.colors


def make_boxplot(ax, groups, labels, colors, title, xlabel):
    bp = ax.boxplot(
        groups,
        labels=labels,
        patch_artist=True,
        medianprops=dict(color="black", linewidth=1.5),
        flierprops=dict(marker=".", markersize=3, alpha=0.5),
    )
    for patch, color in zip(bp["boxes"], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.75)
    ax.set_ylabel("Latency (μs)")
    ax.set_xlabel(xlabel)
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.4)


# ---------------------------------------------------------------------------
# Figure 1 — by subtask, one figure per core, coloured by period
# ---------------------------------------------------------------------------
meta = (
    df.groupby("subtask_id")
    .agg(period_ms=("period_ms", "first"), core=("core", "first"), priority=("priority", "first"))
    .sort_values(["core", "period_ms", "subtask_id"])   # grouped by core first
)

periods = sorted(meta["period_ms"].unique())
period_color = {p: palette[i % len(palette)] for i, p in enumerate(periods)}

# Shared y-axis limit so cores are directly comparable across figures
ymax = df["latency_us"].max() * 1.05

for c in sorted(meta["core"].unique()):
    meta_c = meta[meta["core"] == c]
    subtask_ids = meta_c.index.tolist()
    groups1 = [df[df["subtask_id"] == sid]["latency_us"].values for sid in subtask_ids]
    labels1 = [f"ST {sid}\n{int(meta_c.loc[sid,'period_ms'])}ms" for sid in subtask_ids]
    colors1 = [period_color[meta_c.loc[sid, "period_ms"]] for sid in subtask_ids]

    fig1, ax1 = plt.subplots(figsize=(max(6, len(subtask_ids) * 1.3), 5))
    make_boxplot(ax1, groups1, labels1, colors1,
                 f"Scheduling Latency per Subtask — Core {c}", "Subtask  (period)")
    ax1.set_ylim(0, ymax)
    ax1.legend(
        handles=[mpatches.Patch(facecolor=period_color[p], alpha=0.75, label=f"{int(p)} ms")
                 for p in sorted(meta_c["period_ms"].unique())],
        title="Period", loc="upper right", fontsize=8,
    )
    if note:
        fig1.text(0.99, 0.01, note, ha="right", va="bottom", fontsize=7, color="gray")
    fig1.tight_layout(rect=(0, 0.04, 1, 1))
    fname = f"latency_boxplot_by_subtask_core{c}.png"
    fig1.savefig(fname, dpi=150)
    print(f"Saved {fname}")

# ---------------------------------------------------------------------------
# Figure 2 — by core (sorted by core id), coloured by core
# ---------------------------------------------------------------------------
cores = sorted(df["core"].unique())
core_color = {c: palette[i % len(palette)] for i, c in enumerate(cores)}

groups2 = [df[df["core"] == c]["latency_us"].values for c in cores]
labels2 = [f"Core {c}" for c in cores]
colors2 = [core_color[c] for c in cores]

fig2, ax2 = plt.subplots(figsize=(max(6, len(cores) * 1.8), 5))
make_boxplot(ax2, groups2, labels2, colors2,
             "Scheduling Latency per Core", "CPU Core")
ax2.legend(
    handles=[mpatches.Patch(facecolor=core_color[c], alpha=0.75, label=f"Core {c}") for c in cores],
    title="Core", loc="upper right", fontsize=8,
)
if note:
    fig2.text(0.99, 0.01, note, ha="right", va="bottom", fontsize=7, color="gray")
fig2.tight_layout(rect=(0, 0.04, 1, 1))
fig2.savefig("latency_boxplot_by_core.png", dpi=150)
print("Saved latency_boxplot_by_core.png")

plt.show()
