#!/usr/bin/env bash
#
# run_all.sh — measure the observed WCET of the Malardalen subset used in
# Figure 10 of Li et al. (2024), on a Raspberry Pi 5.
#
# Produces:
#   results/observed_wcet.csv   one summary row per benchmark
#   results/raw_<bench>.csv     every individual run
#   results/env.txt             the machine state the numbers were taken under
#
set -euo pipefail

N=20
WARMUPS=1
CORE=3
OUT=results

usage() {
	cat <<EOF
usage: $0 [-n RUNS] [-w WARMUPS] [-c CORE] [-o OUTDIR]

  -n RUNS     measured executions per benchmark (default $N; use 5 to match the paper)
  -w WARMUPS  discarded executions before measuring (default $WARMUPS)
  -c CORE     core to pin to (default $CORE, the last of the Pi 5's four)
  -o OUTDIR   output directory (default $OUT)
EOF
}

while getopts "n:w:c:o:h" opt; do
	case "$opt" in
		n) N=$OPTARG ;;
		w) WARMUPS=$OPTARG ;;
		c) CORE=$OPTARG ;;
		o) OUT=$OPTARG ;;
		h) usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

cd "$(dirname "$0")"

BENCHES=(insertsort edn fft1 adpcm expint cnt matmult fir bs)

# ---------------------------------------------------------------- privileges
# Everything below is best-effort. Without root the measurement still runs; it
# is just noisier, and the report says so rather than pretending otherwise.
if [ "$(id -u)" -eq 0 ]; then
	SUDO=""
	PRIV=1
elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
	SUDO="sudo"
	PRIV=1
else
	SUDO=""
	PRIV=0
fi

GOV_PATH="/sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor"
OLD_GOV=""
restore_governor() {
	if [ -n "$OLD_GOV" ]; then
		echo "$OLD_GOV" | $SUDO tee "$GOV_PATH" >/dev/null 2>&1 || true
		echo "restored governor: $OLD_GOV" >&2
	fi
}
trap restore_governor EXIT

# ------------------------------------------------------------------- build
echo "==> building (OPT=${OPT:--O0})" >&2
make --no-print-directory OPT="${OPT:--O0}"
mkdir -p "$OUT"

# --------------------------------------------------------------- isolation
FIFO_OK=0
GOV_OK=0
if [ "$PRIV" -eq 1 ]; then
	if [ -w "$GOV_PATH" ] || [ -n "$SUDO" ]; then
		OLD_GOV=$(cat "$GOV_PATH" 2>/dev/null || true)
		if echo performance | $SUDO tee "$GOV_PATH" >/dev/null 2>&1; then
			GOV_OK=1
		else
			OLD_GOV=""
		fi
	fi
	if $SUDO chrt -f 80 true 2>/dev/null; then
		FIFO_OK=1
	fi
fi

RUNNER=(taskset -c "$CORE")
if [ "$FIFO_OK" -eq 1 ]; then
	RUNNER=($SUDO chrt -f 80 taskset -c "$CORE")
fi

if [ "$PRIV" -eq 0 ]; then
	echo "==> no root: running without SCHED_FIFO and without the performance" >&2
	echo "    governor. Results are valid but noisier; re-run with sudo for" >&2
	echo "    tighter numbers." >&2
fi

# --------------------------------------------------------------- environment
{
	echo "# Observed-WCET measurement environment"
	echo "date              : $(date -Is)"
	echo "host              : $(uname -n)"
	echo "kernel            : $(uname -srm)"
	echo "model             : $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
	echo "compiler          : $(${CC:-gcc} --version | head -1)"
	echo "opt level         : ${OPT:--O0}"
	echo "runs (N)          : $N"
	echo "warmups           : $WARMUPS"
	echo "pinned core       : $CORE"
	echo "SCHED_FIFO(80)    : $([ "$FIFO_OK" -eq 1 ] && echo yes || echo 'no (unprivileged)')"
	echo "governor forced   : $([ "$GOV_OK" -eq 1 ] && echo 'performance' || echo "no (left at $(cat "$GOV_PATH" 2>/dev/null || echo unknown))")"
	echo "cpufreq cur/min/max (kHz):" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_cur_freq 2>/dev/null || echo ?)/" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/cpuinfo_min_freq 2>/dev/null || echo ?)/" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/cpuinfo_max_freq 2>/dev/null || echo ?)"
	echo "isolcpus/nohz     : $(cat /proc/cmdline 2>/dev/null || echo unknown)"
	echo
	echo "Timing source: CNTVCT_EL0 (ARMv8-A generic timer), read before and"
	echo "after each execution, per Li et al. 2024 Sec. 5. Cycle columns are"
	echo "derived as ticks * f_cpu / CNTFRQ_EL0, not measured directly."
} > "$OUT/env.txt"

cat "$OUT/env.txt" >&2
echo >&2

# ------------------------------------------------------------------ measure
CSV="$OUT/observed_wcet.csv"
"./bin/${BENCHES[0]}" --header > "$CSV"

for b in "${BENCHES[@]}"; do
	if [ ! -x "./bin/$b" ]; then
		echo "==> skipping $b: binary missing" >&2
		continue
	fi
	"${RUNNER[@]}" "./bin/$b" -n "$N" -w "$WARMUPS" -r "$OUT" >> "$CSV"
done

if [ "$PRIV" -eq 1 ] && [ -n "$SUDO" ]; then
	$SUDO chown -R "$(id -u):$(id -g)" "$OUT" 2>/dev/null || true
fi

# ------------------------------------------------------------------- report
echo >&2
echo "==> $CSV" >&2
awk -F, 'NR==1 { next }
{
	printf "  %-11s  mean %12.3f us  MAX %12.3f us  (max/mean %5.3f)  mean %14.1f cyc  MAX %14.1f cyc\n", \
	       $1, $8, $10, ($8 > 0 ? $10/$8 : 0), $12, $14
}' "$CSV" >&2

cat >&2 <<'EOF'

The "mean" column is the observed WCET as Li et al. define it (they average 5
runs). MAX is the largest execution actually seen and is the more honest
worst-case proxy -- neither is a sound upper bound, since measurement can only
ever report paths that happened to execute.
EOF
