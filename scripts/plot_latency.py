#!/usr/bin/env python3
"""
plot_latency.py — Boxplot of per-job scheduling latency from example_eval output.

Usage:
    python3 plot_latency.py [latency_samples.csv]

Produces two figures:
  latency_boxplot_by_subtask.png  — one box per subtask, coloured by period
  latency_boxplot_by_core.png     — one box per core, coloured by core
"""

import sys
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

csv_path = sys.argv[1] if len(sys.argv) > 1 else "latency_samples.csv"
df = pd.read_csv(csv_path)

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
# Figure 1 — by subtask (sorted by period then id), coloured by period
# ---------------------------------------------------------------------------
meta = (
    df.groupby("subtask_id")
    .agg(period_ms=("period_ms", "first"), core=("core", "first"), priority=("priority", "first"))
    .sort_values(["period_ms", "subtask_id"])
)

periods = sorted(meta["period_ms"].unique())
period_color = {p: palette[i % len(palette)] for i, p in enumerate(periods)}

subtask_ids = meta.index.tolist()
groups1   = [df[df["subtask_id"] == sid]["latency_us"].values for sid in subtask_ids]
labels1   = [f"ST {sid}\n{int(meta.loc[sid,'period_ms'])}ms  C{int(meta.loc[sid,'core'])}" for sid in subtask_ids]
colors1   = [period_color[meta.loc[sid, "period_ms"]] for sid in subtask_ids]

fig1, ax1 = plt.subplots(figsize=(max(9, len(subtask_ids) * 1.3), 5))
make_boxplot(ax1, groups1, labels1, colors1,
             "Scheduling Latency per Subtask", "Subtask  (period · core)")
ax1.legend(
    handles=[mpatches.Patch(facecolor=period_color[p], alpha=0.75, label=f"{int(p)} ms") for p in periods],
    title="Period", loc="upper right", fontsize=8,
)
fig1.tight_layout()
fig1.savefig("latency_boxplot_by_subtask.png", dpi=150)
print("Saved latency_boxplot_by_subtask.png")

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
fig2.tight_layout()
fig2.savefig("latency_boxplot_by_core.png", dpi=150)
print("Saved latency_boxplot_by_core.png")

plt.show()
